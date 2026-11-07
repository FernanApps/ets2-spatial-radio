/*
 * bridge_test.c — Productor de prueba para el bridge de audio externo
 *
 * Genera un tono de 440 Hz y lo escribe en el mapping de memoria compartida.
 * Sirve para probar ETS2CabinDSP.dll sin tener el plugin capturador real.
 *
 * Uso:
 *   bridge_test.exe [--freq 440] [--gain 0.5] [--duration 30]
 *
 * Compilar (Windows, con Zig):
 *   zig cc -target x86_64-windows-gnu -O2 -o bridge_test.exe tools/bridge_test.c
 *
 * Compilar (Windows, con cl.exe):
 *   cl /O2 /Fe:bridge_test.exe tools\bridge_test.c
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../cabin_bridge.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static volatile int g_running = 1;

static BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;
    g_running = 0;
    return TRUE;
}

int main(int argc, char **argv) {
    float freq = 440.0f;
    float gain = 0.5f;
    int duration = 30;

    /* Parsear argumentos */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            freq = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
            gain = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("bridge_test: genera un tono y lo escribe al bridge de audio.\n");
            printf("Uso: bridge_test.exe [--freq 440] [--gain 0.5] [--duration 30]\n");
            printf("  --freq      Frecuencia en Hz (default: 440)\n");
            printf("  --gain      Volumen 0.0-1.0 (default: 0.5)\n");
            printf("  --duration  Duracion en segundos (default: 30)\n");
            return 0;
        }
    }

    printf("Bridge Test: tono %.0f Hz, gain %.2f, duracion %d s\n", freq, gain, duration);
    printf("Presiona Ctrl+C para salir.\n\n");

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    /* Crear el mapping */
    HANDLE handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, CABIN_BRIDGE_SIZE, CABIN_BRIDGE_NAME);
    if (!handle) {
        printf("Error: no se pudo crear el mapping (%lu)\n", GetLastError());
        return 1;
    }

    CabinBridgeBlock *block = (CabinBridgeBlock *)MapViewOfFile(
        handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, CABIN_BRIDGE_SIZE);
    if (!block) {
        printf("Error: no se pudo mapear la memoria (%lu)\n", GetLastError());
        CloseHandle(handle);
        return 1;
    }

    /* Inicializar el header */
    memset(block, 0, sizeof(CabinBridgeBlock));
    block->sample_rate = 48000;
    block->channels = 2;
    block->gain = gain;
    block->enabled = 1;
    block->source_pid = GetCurrentProcessId();
    block->capturing = 1;
    block->write_pos = 0;
    block->read_pos = 0;

    /* Publicar el magic al final para que el consumidor vea un header completo */
    MemoryBarrier();
    block->magic = CABIN_BRIDGE_MAGIC;
    block->version = CABIN_BRIDGE_VERSION;

    printf("Mapping creado: %s\n", CABIN_BRIDGE_NAME);
    printf("Esperando que ETS2CabinDSP.dll lo abra...\n\n");

    /* Generar el tono */
    const float sample_rate = 48000.0f;
    const float omega = 2.0f * (float)M_PI * freq / sample_rate;
    float phase = 0.0f;

    DWORD start_time = GetTickCount();
    DWORD last_status = 0;
    int64_t total_written = 0;

    while (g_running) {
        /* Cuántos frames podemos escribir sin llenar el buffer */
        int64_t avail = (int64_t)CABIN_BRIDGE_CAPACITY - (block->write_pos - block->read_pos);
        if (avail <= 0) {
            /* Buffer lleno, esperar */
            Sleep(5);
            continue;
        }

        /* Escribir hasta 1024 frames por iteración */
        int64_t to_write = avail > 1024 ? 1024 : avail;

        for (int64_t i = 0; i < to_write; ++i) {
            float sample = sinf(phase);
            uint32_t idx = cabin_bridge_index(block->write_pos + i);
            block->data[idx + 0] = sample;  /* L */
            block->data[idx + 1] = sample;  /* R */
            phase += omega;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }

        MemoryBarrier();
        block->write_pos += to_write;
        total_written += to_write;

        /* Status cada segundo */
        DWORD now = GetTickCount();
        if (now - last_status >= 1000) {
            last_status = now;
            DWORD elapsed = (now - start_time) / 1000;
            int64_t consumer_read = block->read_pos;
            int64_t buffered = block->write_pos - consumer_read;
            printf("  [%3lu s] escrito: %lld frames, buffer: %lld, "
                   "playing: %u, underruns: %u\n",
                   elapsed, total_written, buffered,
                   block->playing, block->underruns);

            if (duration > 0 && elapsed >= (DWORD)duration) {
                printf("\nDuracion alcanzada.\n");
                break;
            }
        }

        Sleep(10);  /* ~100 writes/sec, ~102400 samples/sec (más que suficiente para 48kHz) */
    }

    /* Limpiar */
    block->capturing = 0;
    block->enabled = 0;
    MemoryBarrier();

    printf("\nCerrando...\n");
    Sleep(100);

    UnmapViewOfFile(block);
    CloseHandle(handle);

    printf("Bridge cerrado.\n");
    return 0;
}
