/*
 * cabin_bridge.h — Contrato de memoria compartida para audio externo
 *
 * Este header lo incluyen:
 *   - ETS2CabinDSP.dll (consumidor): lee las muestras y las reproduce en FMOD
 *   - El plugin capturador (productor): escribe las muestras desde WASAPI
 *
 * C puro, sin dependencias. Compatible con C++ si se incluye.
 *
 * El productor crea el mapping con CreateFileMappingA; el consumidor lo abre con
 * OpenFileMappingA. Si el mapping no existe, el consumidor asume que no hay
 * productor y no reproduce nada.
 */
#ifndef CABIN_BRIDGE_H
#define CABIN_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Nombre del file mapping en el namespace de la sesión del usuario. */
#define CABIN_BRIDGE_NAME "Local\\ETS2CabinBridge"

/* Magic para validar que el mapping es nuestro y no basura. 'ECB1' en little endian. */
#define CABIN_BRIDGE_MAGIC 0x31424345

/* Versión del layout. Si cambia el struct, subir esto. El consumidor ignora
   versiones que no reconoce. */
#define CABIN_BRIDGE_VERSION 2

/* Capacidad del ring buffer en frames (potencia de 2).
   16384 frames a 48 kHz = ~341 ms de buffer. */
#define CABIN_BRIDGE_CAPACITY 16384

/* Objetivo de latencia en frames (~60 ms a 48 kHz).
   Si write_pos - read_pos supera 2x esto, el consumidor descarta hasta el objetivo. */
#define CABIN_BRIDGE_TARGET_LATENCY 2880

/*
 * Estructura del bloque compartido.
 *
 * Protocolo SPSC (single producer, single consumer):
 *   - El productor escribe: magic, version, sample_rate, channels, gain, enabled,
 *     source_pid, capturing, write_pos, data[].
 *   - El consumidor escribe: read_pos, playing, underruns.
 *   - El productor publica write_pos DESPUÉS de copiar las muestras (MemoryBarrier).
 *   - El consumidor publica read_pos DESPUÉS de consumirlas.
 *
 * Índices monótonos: posición en data = (pos % CABIN_BRIDGE_CAPACITY) * channels.
 */
typedef struct CabinBridgeBlock {
    /* --- Header (el productor escribe una vez al crear) --- */
    uint32_t magic;           /* CABIN_BRIDGE_MAGIC */
    uint32_t version;         /* CABIN_BRIDGE_VERSION */
    uint32_t sample_rate;     /* Hz; debe coincidir con FMOD (48000) */
    uint32_t channels;        /* siempre 2 (estéreo) */

    /* --- Configuración (el productor actualiza) --- */
    float    gain;            /* 0.0 - 2.0, volumen del audio externo */
    uint32_t enabled;         /* 1 = reproducir en la radio */
    uint32_t source_pid;      /* PID del proceso capturado (display) */
    uint32_t capturing;       /* 1 = WASAPI está entregando paquetes */

    /* --- Control de mezcla (el productor actualiza) --- */
    float    radio_volume;    /* 0.0 - 1.0, factor sobre el volumen ORIGINAL (1.0 = sin cambio) */
    uint32_t mute_game_radio; /* 1 = silenciar emisora del juego cuando hay audio externo */
    float    duck_factor;     /* 0.0 - 1.0, factor adicional de atenuación (multiplicativo) */
    uint32_t _reserved1;      /* padding para alineación */

    /* --- Read-back del consumidor (el consumidor escribe) --- */
    uint32_t playing;         /* 1 = el Sound de FMOD está sonando */
    uint32_t underruns;       /* contador de underruns (diagnóstico) */
    float    actual_radio_vol;/* volumen real de la emisora (para debug) */
    uint32_t radio_control_ok;/* 1 = el control de radio está funcionando */

    /* --- Padding para alinear los contadores a 64 bytes (cache line) --- */
    uint8_t  _pad1[16];

    /* --- Ring buffer SPSC --- */
    volatile int64_t write_pos;  /* frames escritos (monótono), alineado */
    uint8_t  _pad2[56];
    volatile int64_t read_pos;   /* frames leídos (monótono), alineado */
    uint8_t  _pad3[56];

    /* Muestras float32 estéreo intercalado (L, R, L, R...).
       Tamaño: CABIN_BRIDGE_CAPACITY * 2 floats = 131072 bytes (~128 KB). */
    float    data[CABIN_BRIDGE_CAPACITY * 2];
} CabinBridgeBlock;

/* Tamaño total del mapping. */
#define CABIN_BRIDGE_SIZE sizeof(CabinBridgeBlock)

/* --- Helpers (inline, opcionales) --- */

/* Frames disponibles para leer. */
static inline int64_t cabin_bridge_available(const CabinBridgeBlock* b) {
    return b->write_pos - b->read_pos;
}

/* Índice en data[] para una posición de frame. */
static inline uint32_t cabin_bridge_index(int64_t frame_pos) {
    return (uint32_t)((frame_pos & (CABIN_BRIDGE_CAPACITY - 1)) * 2);
}

/* Valida el header. Devuelve 1 si es válido, 0 si no. */
static inline int cabin_bridge_valid(const CabinBridgeBlock* b) {
    return b->magic == CABIN_BRIDGE_MAGIC &&
           b->version == CABIN_BRIDGE_VERSION &&
           b->channels == 2;
}

#ifdef __cplusplus
}
#endif

#endif /* CABIN_BRIDGE_H */
