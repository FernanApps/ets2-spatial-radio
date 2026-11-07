/*
 * scs_sdk_c.h — Definiciones mínimas del SCS SDK para C puro
 */
#ifndef SCS_SDK_C_H
#define SCS_SDK_C_H

#include <stdint.h>

typedef uint32_t scs_u32_t;
typedef int32_t  scs_s32_t;
typedef scs_s32_t scs_result_t;

#define SCS_RESULT_ok                  0
#define SCS_RESULT_unsupported        -1
#define SCS_RESULT_invalid_parameter  -2
#define SCS_RESULT_generic_error      -7

#define SCS_TELEMETRY_VERSION_1_00    0x00010000
#define SCS_TELEMETRY_VERSION_1_01    0x00010001
#define SCS_TELEMETRY_VERSION_CURRENT SCS_TELEMETRY_VERSION_1_01

/* Calling convention en Windows */
#ifdef _WIN32
#define SCSAPI_RESULT  scs_result_t __stdcall
#define SCSAPI_VOID    void __stdcall
#else
#define SCSAPI_RESULT  scs_result_t
#define SCSAPI_VOID    void
#endif

/* Parámetros de init - solo necesitamos el puntero a log */
typedef void (*scs_log_t)(int type, const char *msg);

typedef struct {
    scs_log_t log;
    /* otros campos que no usamos */
} scs_sdk_init_params_v100_t;

typedef struct {
    scs_sdk_init_params_v100_t common;
    /* otros campos que no usamos */
} scs_telemetry_init_params_v100_t;

#define SCS_LOG_TYPE_message 0
#define SCS_LOG_TYPE_warning 1
#define SCS_LOG_TYPE_error   2

#endif /* SCS_SDK_C_H */
