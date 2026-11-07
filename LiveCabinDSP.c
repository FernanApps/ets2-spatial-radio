#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* Contrato de memoria compartida para audio externo (Chrome, Spotify, etc.) */
#include "cabin_bridge.h"

/*
 * ETS2 Cabin Radio — 3D Multi-Speaker Truck/Car Cabin Audio Processor
 * (macOS x86_64 & Windows x64)
 *
 * Simulates a physical 4.1-speaker truck/car cabin around the driver's head:
 *   1. Front Left (FL) door/dash speaker
 *   2. Front Right (FR) passenger dash/door speaker (with cross-cab Haas delay)
 *   3. Rear Left (RL) sleeper/B-pillar surround speaker
 *   4. Rear Right (RR) sleeper/B-pillar surround speaker
 *   5. Under-seat / sleeper subwoofer (< 115 Hz cabin pressure)
 *   6. 4-tap windshield / side-glass / sleeper-wall early reflections
 *
 * Every speaker source is spatialized in real time at 48 kHz using:
 *   - Fractional-sample Binaural ITD (Interaural Time Difference)
 *   - 2-band Binaural Head-Shadowing (low diffraction vs. high shadowing)
 *   - Front/back Pinna directivity cues as you rotate the camera 360 deg
 *   - Asymmetric window-open acoustic bleed (wnd_left, wnd_right, cabin_out)
 */

typedef int FMOD_RESULT;
typedef struct FMOD_STUDIO_SYSTEM FMOD_STUDIO_SYSTEM;
typedef struct FMOD_STUDIO_BUS FMOD_STUDIO_BUS;
typedef struct FMOD_SYSTEM FMOD_SYSTEM;
typedef struct FMOD_CHANNELGROUP FMOD_CHANNELGROUP;
typedef struct FMOD_DSP FMOD_DSP;
typedef struct FMOD_SOUND FMOD_SOUND;
typedef struct FMOD_CHANNEL FMOD_CHANNEL;

enum {
    FMOD_OK = 0,
    FMOD_DSP_TYPE_COMPRESSOR = 16,
    FMOD_DSP_TYPE_THREE_EQ = 25,
    FMOD_CHANNELCONTROL_DSP_TAIL = -3,
    /* Flags para CreateSound/CreateStream */
    FMOD_DEFAULT = 0x00000000,
    FMOD_LOOP_NORMAL = 0x00000002,
    FMOD_2D = 0x00000008,
    FMOD_CREATESTREAM = 0x00000080,
    FMOD_OPENUSER = 0x00000100,
    FMOD_SOUND_FORMAT_PCMFLOAT = 5
};

/*
 * FMOD_CREATESOUNDEXINFO para FMOD 2.03.x 64-bit.
 * Usamos offsets fijos en vez de un struct porque el tamaño varía entre versiones.
 * FMOD 2.02+ en 64-bit: cbsize = 0xE0 (224 bytes).
 */
typedef FMOD_RESULT (*FMOD_SOUND_PCMREAD_CALLBACK)(
    FMOD_SOUND *sound, void *data, unsigned int datalen);

/* Offsets en FMOD_CREATESOUNDEXINFO para FMOD 2.x 64-bit */
#define EXINFO_SIZE              224
#define EXINFO_OFF_CBSIZE        0
#define EXINFO_OFF_LENGTH        4
#define EXINFO_OFF_NUMCHANNELS   12
#define EXINFO_OFF_DEFAULTFREQ   16
#define EXINFO_OFF_FORMAT        20
#define EXINFO_OFF_DECODEBUFSIZE 24
#define EXINFO_OFF_PCMREADCB     56
#define EXINFO_OFF_USERDATA      104

enum {
    EQ_LOWGAIN = 0,
    EQ_MIDGAIN = 1,
    EQ_HIGHGAIN = 2,
    EQ_LOWCROSSOVER = 3,
    EQ_HIGHCROSSOVER = 4,
    EQ_CROSSOVERSLOPE = 5
};

enum {
    COMPRESSOR_THRESHOLD = 0,
    COMPRESSOR_RATIO = 1,
    COMPRESSOR_ATTACK = 2,
    COMPRESSOR_RELEASE = 3,
    COMPRESSOR_GAINMAKEUP = 4,
    COMPRESSOR_LINKED = 6
};

typedef FMOD_RESULT (*FMOD_DSP_READ_CALLBACK)(
    void *dsp_state,
    float *inbuffer,
    float *outbuffer,
    unsigned int length,
    int inchannels,
    int *outchannels
);

/* Exact 0xD8 (216-byte) 64-bit layout of FMOD_DSP_DESCRIPTION in FMOD 2.x. */
typedef struct {
    unsigned int pluginsdkversion;
    char name[32];
    unsigned int version;
    int numinputbuffers;
    int numoutputbuffers;
    void *create;
    void *release;
    void *reset;
    FMOD_DSP_READ_CALLBACK read;
    void *process;
    void *setposition;
    int numparameters;
    void *paramdesc;
    void *setparameterfloat;
    void *setparameterint;
    void *setparameterbool;
    void *setparameterdata;
    void *getparameterfloat;
    void *getparameterint;
    void *getparameterbool;
    void *getparameterdata;
    void *shouldiprocess;
    void *userdata;
    void *sys_register;
    void *sys_deregister;
    void *sys_mix;
} FMOD_DSP_DESCRIPTION;

typedef FMOD_RESULT (*StudioUpdateFn)(FMOD_STUDIO_SYSTEM *);
typedef FMOD_RESULT (*StudioGetBusFn)(FMOD_STUDIO_SYSTEM *, const char *, FMOD_STUDIO_BUS **);
typedef FMOD_RESULT (*StudioGetCoreFn)(FMOD_STUDIO_SYSTEM *, FMOD_SYSTEM **);
typedef FMOD_RESULT (*StudioFlushFn)(FMOD_STUDIO_SYSTEM *);
typedef FMOD_RESULT (*StudioGetParamFn)(FMOD_STUDIO_SYSTEM *, const char *, float *, float *);
typedef FMOD_RESULT (*BusLockFn)(FMOD_STUDIO_BUS *);
typedef FMOD_RESULT (*BusGetGroupFn)(FMOD_STUDIO_BUS *, FMOD_CHANNELGROUP **);
typedef FMOD_RESULT (*CreateDSPByTypeFn)(FMOD_SYSTEM *, int, FMOD_DSP **);
typedef FMOD_RESULT (*CreateDSPFn)(FMOD_SYSTEM *, const FMOD_DSP_DESCRIPTION *, FMOD_DSP **);
typedef FMOD_RESULT (*AddDSPFn)(FMOD_CHANNELGROUP *, int, FMOD_DSP *);
typedef FMOD_RESULT (*SetFloatFn)(FMOD_DSP *, int, float);
typedef FMOD_RESULT (*SetIntFn)(FMOD_DSP *, int, int);
typedef FMOD_RESULT (*ReleaseDSPFn)(FMOD_DSP *);
typedef FMOD_RESULT (*GetVersionFn)(FMOD_SYSTEM *, unsigned int *, unsigned int *);

/* Funciones para reproducir audio externo (bridge) */
typedef FMOD_RESULT (*CreateSoundFn)(FMOD_SYSTEM *, const char *, unsigned int,
                                      void *, FMOD_SOUND **);
typedef FMOD_RESULT (*PlaySoundFn)(FMOD_SYSTEM *, FMOD_SOUND *, FMOD_CHANNELGROUP *,
                                    int, FMOD_CHANNEL **);
typedef FMOD_RESULT (*SoundReleaseFn)(FMOD_SOUND *);
typedef FMOD_RESULT (*ChannelStopFn)(FMOD_CHANNEL *);
typedef FMOD_RESULT (*ChannelSetVolumeFn)(FMOD_CHANNEL *, float);
typedef FMOD_RESULT (*ChannelIsPlayingFn)(FMOD_CHANNEL *, int *);
typedef FMOD_RESULT (*SoundSetUserDataFn)(FMOD_SOUND *, void *);
typedef FMOD_RESULT (*SoundGetUserDataFn)(FMOD_SOUND *, void **);

typedef struct {
    StudioGetBusFn get_bus;
    StudioGetCoreFn get_core;
    StudioFlushFn flush;
    StudioGetParamFn get_param;
    BusLockFn lock_bus;
    BusGetGroupFn get_group;
    CreateDSPByTypeFn create_dsp_by_type;
    CreateDSPFn create_dsp;
    AddDSPFn add_dsp;
    SetFloatFn set_float;
    SetIntFn set_int;
    ReleaseDSPFn release_dsp;
    GetVersionFn get_version;
    /* Para audio externo (bridge) */
    CreateSoundFn create_sound;
    PlaySoundFn play_sound;
    SoundReleaseFn sound_release;
    ChannelStopFn channel_stop;
    ChannelSetVolumeFn channel_set_volume;
    ChannelIsPlayingFn channel_is_playing;
    SoundSetUserDataFn sound_set_userdata;
    SoundGetUserDataFn sound_get_userdata;
} CabinAPI;

typedef struct {
    float amount;
    float rear_level;
    float position;
    float low_gain;
    float mid_gain;
    float high_gain;
    float low_crossover;
    float high_crossover;
    float front_gain;
    float rear_gain;
    float sub_gain;
    float reflection_gain;
    float crossfeed;
    float compressor_threshold;
    float compressor_ratio;
} CabinSettings;

/*
 * Target acoustic state updated on the game thread and smoothed sample-by-sample
 * on the FMOD mixer thread for click-free 360-degree head rotation.
 */
typedef struct {
    float delay_L[4];
    float delay_R[4];
    float low_gain_L[4];
    float low_gain_R[4];
    float high_gain_L[4];
    float high_gain_R[4];
    float sub_gain;
    float refl_gain_L;
    float refl_gain_R;
    float dry_blend;
} SpatialTargets;

#define CABIN_DELAY_MASK 4095
#define CABIN_DELAY_SIZE 4096

typedef struct {
    float front_L[CABIN_DELAY_SIZE];
    float front_R[CABIN_DELAY_SIZE];
    float rear_L[CABIN_DELAY_SIZE];
    float rear_R[CABIN_DELAY_SIZE];
    float sub_buf[CABIN_DELAY_SIZE];
    unsigned int write_pos;

    /* One-pole filter states (at 48 kHz) */
    float rear_lp_L;
    float rear_lp_R;
    float spk_lp[4];
    float sub_lp1;
    float sub_lp2;
    float refl_lp_L;
    float refl_lp_R;

    /* Smoothed per-sample parameters */
    SpatialTargets current;
    bool initialized;
} CabinDSPState;

#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
static void lock_state(void) { AcquireSRWLockExclusive(&g_lock); }
static void unlock_state(void) { ReleaseSRWLockExclusive(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_state(void) { pthread_mutex_lock(&g_lock); }
static void unlock_state(void) { pthread_mutex_unlock(&g_lock); }
#endif

static bool g_symbols_resolved = false;
static bool g_installed = false;
static bool g_disabled = false;
static bool g_dry_run = false;
static bool g_hook_logged = false;
static bool g_turn_logged = false;
static bool g_radio_bus_locked = false;
static FMOD_STUDIO_BUS *g_radio_bus = NULL;
static FMOD_DSP *g_eq_dsp = NULL;
static FMOD_DSP *g_spatial_dsp = NULL;
static FMOD_DSP *g_compressor_dsp = NULL;
static CabinSettings g_settings;
static SpatialTargets g_targets;
static CabinDSPState g_dsp_state;
static float g_last_rot = 999.0f;
static float g_last_out = 999.0f;
static float g_last_wnd_l = 999.0f;
static float g_last_wnd_r = 999.0f;
static unsigned int g_retry_counter = 0;
static CabinAPI g_api;
static FILE *g_log = NULL;

/* Estado del bridge de audio externo */
#ifdef _WIN32
static HANDLE g_bridge_handle = NULL;
static CabinBridgeBlock *g_bridge = NULL;
static FMOD_SOUND *g_bridge_sound = NULL;
static FMOD_CHANNEL *g_bridge_channel = NULL;
static FMOD_CHANNELGROUP *g_radio_group = NULL;
static bool g_bridge_enabled = false;
static int64_t g_bridge_read_pos = 0;
static uint32_t g_bridge_underruns = 0;
static unsigned int g_bridge_retry = 0;

/*
 * Direcciones del juego para control de radio (ETS2 1.62.1.0s).
 * Estas direcciones pueden cambiar entre versiones del juego.
 */
#define GAME_BUS_ARRAY_PTR      0x1430b5850
#define GAME_FMOD_STUDIO_PTR    0x1430b5948
#define GAME_FMOD_CORE_PTR      0x1430b5950
#define GAME_RADIO_BUS_INDEX    26

/* Offsets en fmod_bus_t */
#define BUS_OFF_VOLUME          0x04
#define BUS_OFF_MUTE_FLAGS      0x08
#define BUS_OFF_FMOD_BUS        0x20
#define BUS_OFF_CHANNELGROUP    0x48

/* Typedef para Bus::setVolume */
typedef FMOD_RESULT (*BusSetVolumeFn)(void *bus, float volume);
typedef FMOD_RESULT (*BusSetMuteFn)(void *bus, int mute);

static BusSetVolumeFn g_bus_set_volume = NULL;
static BusSetMuteFn g_bus_set_mute = NULL;
static void *g_game_radio_bus = NULL;  /* Puntero al fmod_bus_t del juego */
static float g_original_radio_volume = 1.0f;
static bool g_radio_muted_by_us = false;
#endif

#ifdef __APPLE__
extern FMOD_RESULT FMOD_Studio_System_Update(FMOD_STUDIO_SYSTEM *system);
extern FMOD_RESULT FMOD_CPP_Studio_System_Update(FMOD_STUDIO_SYSTEM *system)
    __asm__("__ZN4FMOD6Studio6System6updateEv");
#endif

static float clamp_float(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void data_folder_path(char *buffer, size_t size) {
#ifdef _WIN32
    const char *local_appdata = getenv("LOCALAPPDATA");
    if (!local_appdata || !*local_appdata) local_appdata = getenv("USERPROFILE");
    if (!local_appdata || !*local_appdata) local_appdata = "C:\\Temp";
    snprintf(buffer, size, "%s\\ETS2 Cabin Radio", local_appdata);
    CreateDirectoryA(buffer, NULL);
#else
    const char *home = NULL;
    struct passwd *account = getpwuid(getuid());
    if (account) home = account->pw_dir;
    if (!home) home = "/tmp";
    snprintf(buffer, size, "%s/Library/Logs/ETS2 Cabin Radio", home);
    mkdir(buffer, 0755);
#endif
}

static void open_log(void) {
    if (g_log) return;

    const char *override = getenv("ETS2_CABIN_LOG_PATH");
    if (override && *override) {
        g_log = fopen(override, "a");
        if (!g_log) g_log = stderr;
        return;
    }

    char folder[PATH_MAX];
    char file[PATH_MAX];
    data_folder_path(folder, sizeof(folder));
#ifdef _WIN32
    snprintf(file, sizeof(file), "%s\\live-dsp.log", folder);
#else
    snprintf(file, sizeof(file), "%s/live-dsp.log", folder);
#endif
    g_log = fopen(file, "a");
    if (!g_log) g_log = stderr;
}

static void cabin_log(const char *format, ...) {
    open_log();
    time_t now = time(NULL);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    fprintf(g_log, "[%s] ", timestamp);

    va_list arguments;
    va_start(arguments, format);
    vfprintf(g_log, format, arguments);
    va_end(arguments);
    fputc('\n', g_log);
    fflush(g_log);
}

static void *load_symbol(const char *name) {
#ifdef _WIN32
    static const char *const modules[] = {
        "fmodstudio.dll",
        "fmodstudioL.dll",
        "fmod.dll",
        "fmodL.dll"
    };
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        HMODULE mod = GetModuleHandleA(modules[i]);
        if (mod) {
            FARPROC proc = GetProcAddress(mod, name);
            if (proc) return (void *)proc;
        }
    }
    cabin_log("Missing symbol %s in FMOD DLLs", name);
    return NULL;
#else
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol) cabin_log("Missing symbol %s: %s", name, dlerror());
    return symbol;
#endif
}

static bool resolve_symbols(void) {
    if (g_symbols_resolved) return true;

    g_api.get_bus = (StudioGetBusFn)load_symbol("FMOD_Studio_System_GetBus");
    g_api.get_core = (StudioGetCoreFn)load_symbol("FMOD_Studio_System_GetCoreSystem");
    g_api.flush = (StudioFlushFn)load_symbol("FMOD_Studio_System_FlushCommands");
    g_api.get_param = (StudioGetParamFn)load_symbol("FMOD_Studio_System_GetParameterByName");
    g_api.lock_bus = (BusLockFn)load_symbol("FMOD_Studio_Bus_LockChannelGroup");
    g_api.get_group = (BusGetGroupFn)load_symbol("FMOD_Studio_Bus_GetChannelGroup");
    g_api.create_dsp_by_type = (CreateDSPByTypeFn)load_symbol("FMOD_System_CreateDSPByType");
    g_api.create_dsp = (CreateDSPFn)load_symbol("FMOD_System_CreateDSP");
    g_api.add_dsp = (AddDSPFn)load_symbol("FMOD_ChannelGroup_AddDSP");
    g_api.set_float = (SetFloatFn)load_symbol("FMOD_DSP_SetParameterFloat");
    g_api.set_int = (SetIntFn)load_symbol("FMOD_DSP_SetParameterInt");
    g_api.release_dsp = (ReleaseDSPFn)load_symbol("FMOD_DSP_Release");
    g_api.get_version = (GetVersionFn)load_symbol("FMOD_System_GetVersion");

    /* Símbolos para audio externo (opcionales: si faltan, solo se desactiva el bridge) */
    g_api.create_sound = (CreateSoundFn)load_symbol("FMOD_System_CreateSound");
    g_api.play_sound = (PlaySoundFn)load_symbol("FMOD_System_PlaySound");
    g_api.sound_release = (SoundReleaseFn)load_symbol("FMOD_Sound_Release");
    g_api.channel_stop = (ChannelStopFn)load_symbol("FMOD_Channel_Stop");
    g_api.channel_set_volume = (ChannelSetVolumeFn)load_symbol("FMOD_Channel_SetVolume");
    g_api.channel_is_playing = (ChannelIsPlayingFn)load_symbol("FMOD_Channel_IsPlaying");
    g_api.sound_set_userdata = (SoundSetUserDataFn)load_symbol("FMOD_Sound_SetUserData");
    g_api.sound_get_userdata = (SoundGetUserDataFn)load_symbol("FMOD_Sound_GetUserData");

    g_symbols_resolved = g_api.get_bus && g_api.get_core && g_api.flush &&
        g_api.lock_bus && g_api.get_group && g_api.create_dsp_by_type &&
        g_api.create_dsp && g_api.add_dsp && g_api.set_float &&
        g_api.set_int && g_api.release_dsp;
    return g_symbols_resolved;
}

static float environment_float(const char *name, float fallback, float minimum, float maximum) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end = NULL;
    float parsed = strtof(value, &end);
    if (end == value || !isfinite(parsed)) return fallback;
    return clamp_float(parsed, minimum, maximum);
}

static CabinSettings build_settings(const char *preset, float amount, float rear_level, float position) {
    amount = clamp_float(amount, 0.0f, 1.0f);
    rear_level = clamp_float(rear_level, 0.0f, 1.0f);
    position = clamp_float(position, -1.0f, 1.0f);
    if (!preset || !*preset) preset = "premium";

    CabinSettings settings = {
        .amount = amount,
        .rear_level = rear_level,
        .position = position,
        .low_gain = 1.6f * amount,
        .mid_gain = -0.6f * amount,
        .high_gain = -0.8f * amount,
        .low_crossover = 220.0f,
        .high_crossover = 5000.0f,
        .front_gain = 0.56f,
        .rear_gain = (0.28f + 0.34f * amount) * rear_level,
        .sub_gain = 0.24f * amount,
        .reflection_gain = 0.22f * amount,
        .crossfeed = 0.16f + 0.12f * amount,
        .compressor_threshold = -10.0f - (5.0f * amount),
        .compressor_ratio = 1.25f + (1.1f * amount)
    };

    if (strcmp(preset, "warm") == 0) {
        settings.low_gain = 2.6f * amount;
        settings.mid_gain = -0.4f * amount;
        settings.high_gain = -1.8f * amount;
        settings.front_gain = 0.52f;
        settings.rear_gain = (0.34f + 0.38f * amount) * rear_level;
        settings.sub_gain = 0.32f * amount;
        settings.reflection_gain = 0.25f * amount;
    } else if (strcmp(preset, "stock") == 0) {
        settings.low_gain = 2.8f * amount;
        settings.mid_gain = -1.5f * amount;
        settings.high_gain = -2.8f * amount;
        settings.front_gain = 0.62f;
        settings.rear_gain = (0.18f + 0.24f * amount) * rear_level;
        settings.sub_gain = 0.14f * amount;
        settings.reflection_gain = 0.28f * amount;
        settings.compressor_ratio = 1.5f + (1.4f * amount);
    } else if (strcmp(preset, "spatial") == 0) {
        settings.low_gain = 1.5f * amount;
        settings.mid_gain = -0.8f * amount;
        settings.high_gain = -0.6f * amount;
        settings.front_gain = 0.50f;
        settings.rear_gain = (0.38f + 0.44f * amount) * rear_level;
        settings.sub_gain = 0.28f * amount;
        settings.reflection_gain = 0.31f * amount;
        settings.crossfeed = 0.22f + 0.14f * amount;
    }

    cabin_log("Preset=%s intensity=%.2f rear=%.2f position=%.2f",
              preset, amount, rear_level, position);
    return settings;
}

static void live_settings_path(char *buffer, size_t size) {
    char folder[PATH_MAX];
    data_folder_path(folder, sizeof(folder));
#ifdef _WIN32
    snprintf(buffer, size, "%s\\live-settings.env", folder);
#else
    snprintf(buffer, size, "%s/live-settings.env", folder);
#endif
}

static CabinSettings read_settings(void) {
    float amount = environment_float("ETS2_CABIN_INTENSITY", 0.72f, 0.0f, 1.0f);
    float rear_level = environment_float("ETS2_CABIN_REAR", 0.75f, 0.0f, 1.0f);
    float position = environment_float("ETS2_CABIN_POSITION", 0.12f, -1.0f, 1.0f);
    char preset[32] = "premium";
    const char *env_preset = getenv("ETS2_CABIN_PRESET");
    if (env_preset && *env_preset) {
        snprintf(preset, sizeof(preset), "%s", env_preset);
    }

    char path[PATH_MAX];
    live_settings_path(path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (file) {
        char line[128];
        while (fgets(line, sizeof(line), file)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (strncmp(line, "preset=", 7) == 0 && line[7]) {
                snprintf(preset, sizeof(preset), "%s", line + 7);
            } else if (strncmp(line, "intensity=", 10) == 0) {
                amount = strtof(line + 10, NULL);
            } else if (strncmp(line, "rear=", 5) == 0) {
                rear_level = strtof(line + 5, NULL);
            } else if (strncmp(line, "position=", 9) == 0) {
                position = strtof(line + 9, NULL);
            }
        }
        fclose(file);
    }

    return build_settings(preset, amount, rear_level, position);
}

/*
 * Fractional sample reader from a 4096-sample circular buffer.
 */
static inline float read_delay_frac(const float *buf, unsigned int write_pos, float delay_samples) {
    if (delay_samples < 1.0f) delay_samples = 1.0f;
    if (delay_samples > (float)(CABIN_DELAY_SIZE - 4)) delay_samples = (float)(CABIN_DELAY_SIZE - 4);
    int d_int = (int)delay_samples;
    float frac = delay_samples - (float)d_int;
    unsigned int idx0 = (write_pos - (unsigned int)d_int) & CABIN_DELAY_MASK;
    unsigned int idx1 = (idx0 - 1u) & CABIN_DELAY_MASK;
    return buf[idx0] + frac * (buf[idx1] - buf[idx0]);
}

/*
 * Real-time 48 kHz 3D Multi-Speaker Cabin DSP callback.
 * Simulates Front L/R (dash + door), Rear L/R (sleeper/B-pillar), Under-seat Sub,
 * and 4-tap cabin glass/wall reflections with continuous Binaural ITD & ILD.
 */
static FMOD_RESULT cabin_spatial_dsp_read(
    void *dsp_state,
    float *inbuffer,
    float *outbuffer,
    unsigned int length,
    int inchannels,
    int *outchannels
) {
    (void)dsp_state;
    if (outchannels) *outchannels = inchannels;
    if (!inbuffer || !outbuffer || inchannels < 1) return FMOD_OK;

    if (inchannels == 1) {
        memcpy(outbuffer, inbuffer, length * sizeof(float));
        return FMOD_OK;
    }

    CabinDSPState *st = &g_dsp_state;
    SpatialTargets tgt = g_targets;
    if (!st->initialized) {
        st->current = tgt;
        st->initialized = true;
    }

    /* Filter coefficients at 48 kHz:
     *   rear_alpha (~3.2 kHz): warm rear sleeper/B-pillar speaker cone roll-off
     *   split_alpha (~850 Hz): separates diffracting low/body from directional highs
     *   sub_alpha (~115 Hz):   under-seat / sleeper subwoofer crossover
     *   refl_alpha (~2.5 kHz): cabin upholstery / glass reflection damping
     */
    const float rear_alpha = 0.34f;
    const float split_alpha = 0.105f;
    const float sub_alpha = 0.015f;
    const float refl_alpha = 0.28f;
    const float smooth = 0.0025f;
    const float cross = g_settings.crossfeed;

    for (unsigned int i = 0; i < length; i++) {
        float in_L = inbuffer[i * inchannels + 0];
        float in_R = inbuffer[i * inchannels + 1];

        /* Smoothly interpolate spatial targets sample-by-sample (click-free head turns). */
        for (int s = 0; s < 4; s++) {
            st->current.delay_L[s]     += smooth * (tgt.delay_L[s]     - st->current.delay_L[s]);
            st->current.delay_R[s]     += smooth * (tgt.delay_R[s]     - st->current.delay_R[s]);
            st->current.low_gain_L[s]  += smooth * (tgt.low_gain_L[s]  - st->current.low_gain_L[s]);
            st->current.low_gain_R[s]  += smooth * (tgt.low_gain_R[s]  - st->current.low_gain_R[s]);
            st->current.high_gain_L[s] += smooth * (tgt.high_gain_L[s] - st->current.high_gain_L[s]);
            st->current.high_gain_R[s] += smooth * (tgt.high_gain_R[s] - st->current.high_gain_R[s]);
        }
        st->current.sub_gain    += smooth * (tgt.sub_gain    - st->current.sub_gain);
        st->current.refl_gain_L += smooth * (tgt.refl_gain_L - st->current.refl_gain_L);
        st->current.refl_gain_R += smooth * (tgt.refl_gain_R - st->current.refl_gain_R);
        st->current.dry_blend   += smooth * (tgt.dry_blend   - st->current.dry_blend);

        unsigned int wp = st->write_pos;

        /* 1. Feed Front Left & Front Right speaker buffers. */
        st->front_L[wp] = in_L;
        st->front_R[wp] = in_R;

        /* 2. Feed Rear Left & Rear Right sleeper/cabin speaker buffers with warm contour + acoustic cross-blend. */
        float rear_src_L = in_L * (1.0f - cross) + in_R * cross;
        float rear_src_R = in_R * (1.0f - cross) + in_L * cross;
        st->rear_lp_L += rear_alpha * (rear_src_L - st->rear_lp_L);
        st->rear_lp_R += rear_alpha * (rear_src_R - st->rear_lp_R);
        st->rear_L[wp] = st->rear_lp_L + 0.55f * (rear_src_L - st->rear_lp_L);
        st->rear_R[wp] = st->rear_lp_R + 0.55f * (rear_src_R - st->rear_lp_R);

        /* 3. Feed Under-Seat / Bunk Subwoofer (2nd-order 115 Hz low-pass). */
        float mono = 0.5f * (in_L + in_R);
        st->sub_lp1 += sub_alpha * (mono - st->sub_lp1);
        st->sub_lp2 += sub_alpha * (st->sub_lp1 - st->sub_lp2);
        st->sub_buf[wp] = st->sub_lp2;

        /* 4. Render all 4 physical speakers (FL=0, FR=1, RL=2, RR=3) with Binaural ITD & 2-band ILD. */
        const float *spk_bufs[4] = { st->front_L, st->front_R, st->rear_L, st->rear_R };
        float ear_L = 0.0f;
        float ear_R = 0.0f;

        for (int s = 0; s < 4; s++) {
            float sample_L = read_delay_frac(spk_bufs[s], wp, st->current.delay_L[s]);
            float sample_R = read_delay_frac(spk_bufs[s], wp, st->current.delay_R[s]);

            /* Update speaker low/body crossover state (~850 Hz). */
            float avg_s = 0.5f * (sample_L + sample_R);
            st->spk_lp[s] += split_alpha * (avg_s - st->spk_lp[s]);
            float low_s = st->spk_lp[s];
            float high_L = sample_L - low_s;
            float high_R = sample_R - low_s;

            ear_L += low_s * st->current.low_gain_L[s] + high_L * st->current.high_gain_L[s];
            ear_R += low_s * st->current.low_gain_R[s] + high_R * st->current.high_gain_R[s];
        }

        /* 5. Add Under-Seat Subwoofer (~2.8 ms propagation delay = 134 samples). */
        float sub_sig = st->sub_buf[(wp - 134u) & CABIN_DELAY_MASK] * st->current.sub_gain;
        ear_L += sub_sig;
        ear_R += sub_sig;

        /* 6. 4-Tap Cabin Early Reflections (Windshield 11.5ms, Side Glass 18.8ms, Roof 28.1ms, Sleeper Wall 41.2ms). */
        float tap1_L = st->front_L[(wp - 552u)  & CABIN_DELAY_MASK];
        float tap1_R = st->front_R[(wp - 576u)  & CABIN_DELAY_MASK];
        float tap2_L = st->front_R[(wp - 902u)  & CABIN_DELAY_MASK];
        float tap2_R = st->front_L[(wp - 936u)  & CABIN_DELAY_MASK];
        float tap3_L = st->rear_L[(wp - 1348u)  & CABIN_DELAY_MASK];
        float tap3_R = st->rear_R[(wp - 1392u)  & CABIN_DELAY_MASK];
        float tap4_L = st->rear_R[(wp - 1940u)  & CABIN_DELAY_MASK];
        float tap4_R = st->rear_L[(wp - 1996u)  & CABIN_DELAY_MASK];

        float raw_refl_L = 0.40f * tap1_L + 0.30f * tap2_L + 0.20f * tap3_L + 0.14f * tap4_L;
        float raw_refl_R = 0.40f * tap1_R + 0.30f * tap2_R + 0.20f * tap3_R + 0.14f * tap4_R;
        st->refl_lp_L += refl_alpha * (raw_refl_L - st->refl_lp_L);
        st->refl_lp_R += refl_alpha * (raw_refl_R - st->refl_lp_R);

        ear_L += st->refl_lp_L * st->current.refl_gain_L;
        ear_R += st->refl_lp_R * st->current.refl_gain_R;

        /* Blend with dry signal when Cabin Effect slider is lowered toward 0. */
        float dry = st->current.dry_blend;
        float wet = 1.0f - dry;
        float out_L = dry * in_L + wet * ear_L;
        float out_R = dry * in_R + wet * ear_R;

        outbuffer[i * inchannels + 0] = out_L;
        outbuffer[i * inchannels + 1] = out_R;
        for (int c = 2; c < inchannels; c++) {
            outbuffer[i * inchannels + c] = inbuffer[i * inchannels + c];
        }

        st->write_pos = (wp + 1u) & CABIN_DELAY_MASK;
    }

    return FMOD_OK;
}

static FMOD_DSP *create_effect_by_type(FMOD_SYSTEM *core, int type, const char *name) {
    FMOD_DSP *effect = NULL;
    FMOD_RESULT result = g_api.create_dsp_by_type(core, type, &effect);
    if (result != FMOD_OK || !effect) {
        cabin_log("Could not create %s DSP (FMOD result %d)", name, result);
        return NULL;
    }
    return effect;
}

static FMOD_DSP *create_spatial_cabin_dsp(FMOD_SYSTEM *core) {
    static FMOD_DSP_DESCRIPTION desc;
    memset(&desc, 0, sizeof(desc));
    desc.pluginsdkversion = 0x00010000;
    snprintf(desc.name, sizeof(desc.name), "ETS2 3D Cabin Spatializer");
    desc.version = 0x00010200;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = cabin_spatial_dsp_read;

    FMOD_DSP *dsp = NULL;
    FMOD_RESULT result = g_api.create_dsp(core, &desc, &dsp);
    if (result != FMOD_OK || !dsp) {
        cabin_log("Could not create 3D Cabin Spatializer DSP (FMOD result %d)", result);
        return NULL;
    }
    return dsp;
}

static bool attach_effect(FMOD_CHANNELGROUP *group, FMOD_DSP *effect, const char *name) {
    FMOD_RESULT result = g_api.add_dsp(group, FMOD_CHANNELCONTROL_DSP_TAIL, effect);
    if (result != FMOD_OK) {
        cabin_log("Could not attach %s DSP (FMOD result %d)", name, result);
        g_api.release_dsp(effect);
        return false;
    }
    cabin_log("Attached %s DSP", name);
    return true;
}

static void configure_eq(FMOD_DSP *effect, CabinSettings s) {
    g_api.set_float(effect, EQ_LOWGAIN, s.low_gain);
    g_api.set_float(effect, EQ_MIDGAIN, s.mid_gain);
    g_api.set_float(effect, EQ_HIGHGAIN, s.high_gain);
    g_api.set_float(effect, EQ_LOWCROSSOVER, s.low_crossover);
    g_api.set_float(effect, EQ_HIGHCROSSOVER, s.high_crossover);
    g_api.set_int(effect, EQ_CROSSOVERSLOPE, 1);
}

static void configure_compressor(FMOD_DSP *effect, CabinSettings s) {
    g_api.set_float(effect, COMPRESSOR_THRESHOLD, s.compressor_threshold);
    g_api.set_float(effect, COMPRESSOR_RATIO, s.compressor_ratio);
    g_api.set_float(effect, COMPRESSOR_ATTACK, 20.0f);
    g_api.set_float(effect, COMPRESSOR_RELEASE, 220.0f);
    g_api.set_float(effect, COMPRESSOR_GAINMAKEUP, 0.6f);
    g_api.set_int(effect, COMPRESSOR_LINKED, 1);
}

static float read_studio_param(FMOD_STUDIO_SYSTEM *studio, const char *name, float fallback) {
    if (!g_api.get_param) return fallback;
    float value = fallback;
    float final_value = fallback;
    if (g_api.get_param(studio, name, &value, &final_value) == FMOD_OK && isfinite(final_value)) {
        return final_value;
    }
    return fallback;
}

/*
 * Computes binaural delays (ITD) and 2-band head-shadowing gains (ILD + Pinna)
 * for all 4 cabin speakers (Front L, Front R, Rear L, Rear R) given the driver's
 * current head rotation (rot_rad) and window state.
 */
static void compute_spatial_targets(float rot_rad, float cabin_out, float wnd_left, float wnd_right) {
    CabinSettings s = g_settings;
    float pos = s.position;
    float window_open = 0.5f * (wnd_left + wnd_right);
    float exterior_atten = 1.0f - 0.55f * cabin_out;

    /*
     * Physical speaker azimuths (radians) around the driver's seat in cabin coordinates:
     *   0: Front Left  (FL) — driver door / A-pillar (-33 deg)
     *   1: Front Right (FR) — passenger dash / door (+50 deg, further across cab)
     *   2: Rear Left   (RL) — sleeper / rear B-pillar behind driver (-136 deg)
     *   3: Rear Right  (RR) — sleeper / rear B-pillar behind passenger (+140 deg)
     */
    const float spk_angles[4] = {
        -0.58f + (0.28f * pos),
        +0.87f + (0.28f * pos),
        -2.37f + (0.14f * pos),
        +2.44f + (0.14f * pos)
    };

    /*
     * Base acoustic propagation delays in samples at 48 kHz (1 ms = 48 samples):
     *   FL is closest (~0.95 ms), FR is across the dash (~2.35 ms),
     *   RL is behind driver (~3.55 ms), RR is diagonally behind in sleeper (~4.85 ms).
     *   Shifting Dashboard Position left/right shifts the FL vs. FR arrival time!
     */
    const float base_delays[4] = {
        46.0f  + (28.0f * pos),
        113.0f - (28.0f * pos),
        170.0f + (14.0f * pos),
        232.0f - (14.0f * pos)
    };

    const float base_gains[4] = {
        s.front_gain * (1.02f - 0.10f * pos) * exterior_atten,
        s.front_gain * (0.92f + 0.10f * pos) * exterior_atten,
        s.rear_gain  * (0.98f - 0.06f * pos) * exterior_atten,
        s.rear_gain  * (0.90f + 0.06f * pos) * exterior_atten
    };

    SpatialTargets next;
    memset(&next, 0, sizeof(next));

    for (int k = 0; k < 4; k++) {
        /* Angle of speaker k relative to driver's nose:
         * Turning head left (rot_rad < 0) increases rel_angle -> shifts sound toward right ear (+).
         * Turning head right (rot_rad > 0) decreases rel_angle -> shifts sound toward left ear (-).
         */
        float rel_angle = spk_angles[k] - rot_rad;
        float az = sinf(rel_angle);        /* -1 = directly left ear, +1 = directly right ear */
        float front_cos = cosf(rel_angle); /* +1 = directly in front of face, -1 = directly behind head */

        /* Binaural Interaural Time Difference (ITD): +/- 15 samples (~0.62 ms interaural span). */
        float itd = 15.0f * az;
        next.delay_L[k] = base_delays[k] + itd;
        next.delay_R[k] = base_delays[k] - itd;

        /* Pinna front/back directivity: sound sources in front of the face have full treble presence;
         * sound sources behind the head have natural pinna high-frequency rolloff.
         * Turning your head toward the rear speakers brings their front_cos > 0, opening them up!
         */
        float front_factor = 0.5f * (1.0f + front_cos); /* 0 behind head, 1 in front of face */
        float pinna_high = 0.56f + 0.44f * front_factor;
        float pinna_low  = 0.86f + 0.14f * front_factor;

        /* Binaural Head-Shadowing (ILD):
         * Low frequencies (< 850 Hz) diffract around the head (mild ILD).
         * High frequencies (> 850 Hz) are strongly shadowed by the head on the opposite ear.
         */
        float pan_strength = 0.55f + 0.35f * s.amount;
        float low_ild_L  = clamp_float(1.0f - (0.34f * pan_strength * az), 0.45f, 1.35f);
        float low_ild_R  = clamp_float(1.0f + (0.34f * pan_strength * az), 0.45f, 1.35f);
        float high_ild_L = clamp_float(1.0f - (0.78f * pan_strength * az), 0.16f, 1.55f);
        float high_ild_R = clamp_float(1.0f + (0.78f * pan_strength * az), 0.16f, 1.55f);

        next.low_gain_L[k]  = base_gains[k] * pinna_low  * low_ild_L;
        next.low_gain_R[k]  = base_gains[k] * pinna_low  * low_ild_R;
        next.high_gain_L[k] = base_gains[k] * pinna_high * high_ild_L;
        next.high_gain_R[k] = base_gains[k] * pinna_high * high_ild_R;
    }

    /* Under-seat subwoofer loses cabin pressure when windows or doors/camera are open. */
    float cabin_seal = clamp_float(1.0f - 0.45f * window_open - 0.65f * cabin_out, 0.15f, 1.0f);
    next.sub_gain = s.sub_gain * cabin_seal;

    /* Asymmetric side-glass reflections: rolling down left window reduces left glass reflection. */
    float refl_seal_L = clamp_float(1.0f - 0.55f * wnd_left  - 0.70f * cabin_out, 0.10f, 1.0f);
    float refl_seal_R = clamp_float(1.0f - 0.55f * wnd_right - 0.70f * cabin_out, 0.10f, 1.0f);
    next.refl_gain_L = s.reflection_gain * refl_seal_L;
    next.refl_gain_R = s.reflection_gain * refl_seal_R;

    /* Dry blend only when user lowers Cabin Effect below 25%. */
    next.dry_blend = clamp_float(1.0f - (s.amount * 1.15f), 0.0f, 0.80f);

    g_targets = next;
}

/* ========================================================================
 * AUDIO BRIDGE: reproduce audio externo (Chrome, Spotify, etc.) en bus:/radio
 * ======================================================================== */
#ifdef _WIN32

static void bridge_close(void) {
    if (g_bridge_channel && g_api.channel_stop) {
        g_api.channel_stop(g_bridge_channel);
    }
    g_bridge_channel = NULL;

    if (g_bridge_sound && g_api.sound_release) {
        g_api.sound_release(g_bridge_sound);
    }
    g_bridge_sound = NULL;

    if (g_bridge) {
        UnmapViewOfFile(g_bridge);
        g_bridge = NULL;
    }
    if (g_bridge_handle) {
        CloseHandle(g_bridge_handle);
        g_bridge_handle = NULL;
    }
    g_bridge_enabled = false;
    g_bridge_read_pos = 0;
}

static bool bridge_open(void) {
    if (g_bridge) return true;

    g_bridge_handle = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                        CABIN_BRIDGE_NAME);
    if (!g_bridge_handle) return false;

    g_bridge = (CabinBridgeBlock *)MapViewOfFile(
        g_bridge_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, CABIN_BRIDGE_SIZE);
    if (!g_bridge) {
        CloseHandle(g_bridge_handle);
        g_bridge_handle = NULL;
        return false;
    }

    if (!cabin_bridge_valid(g_bridge)) {
        cabin_log("Bridge: invalid magic/version/channels, ignoring.");
        UnmapViewOfFile(g_bridge);
        g_bridge = NULL;
        CloseHandle(g_bridge_handle);
        g_bridge_handle = NULL;
        return false;
    }

    g_bridge_read_pos = g_bridge->write_pos;  /* empezar desde el final, sin latencia acumulada */
    cabin_log("Bridge: connected to external audio source (pid %u).", g_bridge->source_pid);
    return true;
}

/*
 * Callback de FMOD: lee muestras del ring buffer.
 * Corre en el hilo del mixer, sin locks, sin logs, sin malloc.
 */
static FMOD_RESULT bridge_pcm_read(FMOD_SOUND *sound, void *data,
                                    unsigned int datalen) {
    (void)sound;
    float *out = (float *)data;
    const unsigned int frames = datalen / (2 * sizeof(float));

    if (!g_bridge || !g_bridge->enabled || !g_bridge->capturing) {
        /* Silencio */
        for (unsigned int i = 0; i < frames * 2; ++i) out[i] = 0.0f;
        return FMOD_OK;
    }

    int64_t avail = cabin_bridge_available(g_bridge);
    if (avail <= 0) {
        /* Underrun: no hay datos */
        for (unsigned int i = 0; i < frames * 2; ++i) out[i] = 0.0f;
        g_bridge_underruns++;
        return FMOD_OK;
    }

    /* Control de deriva: si hay demasiado buffer, descartamos hasta el objetivo */
    if (avail > (int64_t)(CABIN_BRIDGE_TARGET_LATENCY * 2)) {
        int64_t skip = avail - CABIN_BRIDGE_TARGET_LATENCY;
        g_bridge_read_pos += skip;
        avail = CABIN_BRIDGE_TARGET_LATENCY;
    }

    const float gain = g_bridge->gain;
    unsigned int copied = 0;
    while (copied < frames && avail > 0) {
        uint32_t idx = cabin_bridge_index(g_bridge_read_pos);
        out[copied * 2 + 0] = g_bridge->data[idx + 0] * gain;
        out[copied * 2 + 1] = g_bridge->data[idx + 1] * gain;
        g_bridge_read_pos++;
        avail--;
        copied++;
    }

    /* Rellenar el resto con silencio si no hubo suficiente */
    for (unsigned int i = copied; i < frames; ++i) {
        out[i * 2 + 0] = 0.0f;
        out[i * 2 + 1] = 0.0f;
    }

    /* Publicar read_pos para que el productor sepa cuánto consumimos */
    MemoryBarrier();
    g_bridge->read_pos = g_bridge_read_pos;

    return FMOD_OK;
}

static bool bridge_try_start(FMOD_SYSTEM *core) {
    if (!g_api.create_sound || !g_api.play_sound || !g_radio_group) {
        return false;
    }

    if (g_bridge_sound) return true;  /* ya creado */

    /* Construir FMOD_CREATESOUNDEXINFO manualmente con offsets fijos */
    char exinfo[EXINFO_SIZE];
    memset(exinfo, 0, EXINFO_SIZE);

    /* cbsize: tamaño del struct */
    *(int *)(exinfo + EXINFO_OFF_CBSIZE) = EXINFO_SIZE;
    /* length: tamaño en bytes del audio virtual (2 segundos) */
    *(unsigned int *)(exinfo + EXINFO_OFF_LENGTH) = 48000 * 2 * sizeof(float) * 2;
    /* numchannels */
    *(int *)(exinfo + EXINFO_OFF_NUMCHANNELS) = 2;
    /* defaultfrequency */
    *(int *)(exinfo + EXINFO_OFF_DEFAULTFREQ) = 48000;
    /* format = FMOD_SOUND_FORMAT_PCMFLOAT */
    *(int *)(exinfo + EXINFO_OFF_FORMAT) = FMOD_SOUND_FORMAT_PCMFLOAT;
    /* decodebuffersize */
    *(unsigned int *)(exinfo + EXINFO_OFF_DECODEBUFSIZE) = 1024;
    /* pcmreadcallback */
    *(FMOD_SOUND_PCMREAD_CALLBACK *)(exinfo + EXINFO_OFF_PCMREADCB) = bridge_pcm_read;

    cabin_log("Bridge: creating sound - exinfo=%d bytes, core=%p, group=%p",
              EXINFO_SIZE, (void*)core, (void*)g_radio_group);

    /* Intentar con PCMFLOAT primero */
    FMOD_RESULT result = g_api.create_sound(
        core, "",  /* nombre vacío para OPENUSER */
        FMOD_OPENUSER | FMOD_CREATESTREAM | FMOD_LOOP_NORMAL | FMOD_2D,
        exinfo, &g_bridge_sound);

    if (result != FMOD_OK || !g_bridge_sound) {
        cabin_log("Bridge: PCMFLOAT failed (err %d), trying PCM16...", result);

        /* Fallback a PCM16 que tiene mejor compatibilidad */
        memset(exinfo, 0, EXINFO_SIZE);
        *(int *)(exinfo + EXINFO_OFF_CBSIZE) = EXINFO_SIZE;
        *(unsigned int *)(exinfo + EXINFO_OFF_LENGTH) = 48000 * 2 * 2 * 2; /* 2 bytes per sample */
        *(int *)(exinfo + EXINFO_OFF_NUMCHANNELS) = 2;
        *(int *)(exinfo + EXINFO_OFF_DEFAULTFREQ) = 48000;
        *(int *)(exinfo + EXINFO_OFF_FORMAT) = 2;  /* FMOD_SOUND_FORMAT_PCM16 */
        *(unsigned int *)(exinfo + EXINFO_OFF_DECODEBUFSIZE) = 1024;
        *(FMOD_SOUND_PCMREAD_CALLBACK *)(exinfo + EXINFO_OFF_PCMREADCB) = bridge_pcm_read;

        result = g_api.create_sound(
            core, "",
            FMOD_OPENUSER | FMOD_CREATESTREAM | FMOD_LOOP_NORMAL | FMOD_2D,
            exinfo, &g_bridge_sound);

        if (result != FMOD_OK || !g_bridge_sound) {
            cabin_log("Bridge: PCM16 also failed (err %d). Cannot create stream.", result);
            return false;
        }
        cabin_log("Bridge: PCM16 fallback succeeded.");
    }

    result = g_api.play_sound(core, g_bridge_sound, g_radio_group, 0, &g_bridge_channel);
    if (result != FMOD_OK || !g_bridge_channel) {
        cabin_log("Bridge: could not play Sound (FMOD result %d).", result);
        g_api.sound_release(g_bridge_sound);
        g_bridge_sound = NULL;
        return false;
    }

    if (g_api.channel_set_volume) {
        g_api.channel_set_volume(g_bridge_channel, 1.0f);
    }

    cabin_log("Bridge: external audio playing on bus:/radio.");
    g_bridge_enabled = true;
    return true;
}

/* Forward declarations para control de radio */
static bool radio_control_init(void);
static void radio_set_game_volume(float volume);
static void radio_mute_game_station(bool mute);

/*
 * Llamada desde cabin_after_update: revisa si hay un productor conectado.
 */
static void bridge_update(FMOD_SYSTEM *core) {
    /* Reintentar abrir el mapping cada ~2 segundos */
    if (!g_bridge) {
        if ((g_bridge_retry++ % 60) == 0) {
            if (bridge_open()) {
                bridge_try_start(core);
            }
        }
        return;
    }

    /* Inicializar control de radio si no está listo */
    static bool radio_init_tried = false;
    if (!radio_init_tried) {
        radio_init_tried = true;
        radio_control_init();
    }

    /* Verificar que el productor sigue vivo */
    bool was_enabled = g_bridge_enabled;
    if (!g_bridge->capturing) {
        /* El productor dejó de capturar; lo consideramos desconectado */
        if (g_bridge_enabled) {
            cabin_log("Bridge: producer stopped capturing.");
            g_bridge_enabled = false;
        }
    } else if (!g_bridge_enabled && g_bridge_sound) {
        /* El productor volvió; reactivar */
        g_bridge_enabled = true;
        cabin_log("Bridge: producer resumed.");
    }

    /* Controlar la emisora del juego según parámetros del bridge */
    if (g_bridge_enabled) {
        if (g_bridge->mute_game_radio) {
            /* Silenciar completamente la emisora */
            radio_mute_game_station(true);
        } else if (g_bridge->radio_volume >= 0.0f) {
            /* Aplicar el volumen especificado (con duck_factor si está activo) */
            float target_vol = g_bridge->radio_volume;
            if (g_bridge->duck_factor > 0.0f && g_bridge->duck_factor < 1.0f) {
                target_vol *= g_bridge->duck_factor;
            }
            radio_set_game_volume(target_vol);
        }
    } else if (was_enabled) {
        /* Audio externo paró: restaurar la emisora del juego */
        radio_mute_game_station(false);
    }

    /* Volumen del canal de audio externo */
    if (g_bridge_channel && g_api.channel_set_volume) {
        float ext_vol = g_bridge->gain;
        if (ext_vol < 0.0f) ext_vol = 0.0f;
        if (ext_vol > 2.0f) ext_vol = 2.0f;
        g_api.channel_set_volume(g_bridge_channel, ext_vol);
    }

    /* Write-back del estado para el productor */
    if (g_bridge_channel && g_api.channel_is_playing) {
        int playing = 0;
        g_api.channel_is_playing(g_bridge_channel, &playing);
        g_bridge->playing = playing ? 1 : 0;
    }
    g_bridge->underruns = g_bridge_underruns;
    g_bridge->actual_radio_vol = g_radio_muted_by_us ? 0.0f : g_original_radio_volume;
    g_bridge->radio_control_ok = g_game_radio_bus ? 1 : 0;
}

/*
 * Control del bus de radio del juego.
 * Permite silenciar la emisora cuando hay audio externo.
 */
static bool radio_control_init(void) {
    if (g_game_radio_bus) return true;

    /* Intentar leer el array de buses del juego */
    uint64_t *bus_array_ptr = (uint64_t *)GAME_BUS_ARRAY_PTR;
    if (!bus_array_ptr || !*bus_array_ptr) {
        return false;
    }

    uint64_t bus_array = *bus_array_ptr;
    uint64_t *radio_bus_ptr = (uint64_t *)(bus_array + GAME_RADIO_BUS_INDEX * 8);
    if (!radio_bus_ptr || !*radio_bus_ptr) {
        return false;
    }

    g_game_radio_bus = (void *)*radio_bus_ptr;

    /* Leer el volumen actual */
    float *vol_ptr = (float *)((char *)g_game_radio_bus + BUS_OFF_VOLUME);
    g_original_radio_volume = *vol_ptr;

    /* Cargar las funciones de FMOD para controlar el bus */
    g_bus_set_volume = (BusSetVolumeFn)load_symbol("FMOD_Studio_Bus_SetVolume");
    g_bus_set_mute = (BusSetMuteFn)load_symbol("FMOD_Studio_Bus_SetMute");

    cabin_log("Radio control: initialized (bus=%p, vol=%.2f)",
              g_game_radio_bus, g_original_radio_volume);
    return true;
}

static void radio_set_game_volume(float volume) {
    if (!g_game_radio_bus || !g_bus_set_volume) return;

    /* Obtener el FMOD::Studio::Bus* */
    void **fmod_bus_ptr = (void **)((char *)g_game_radio_bus + BUS_OFF_FMOD_BUS);
    if (!fmod_bus_ptr || !*fmod_bus_ptr) return;

    g_bus_set_volume(*fmod_bus_ptr, volume);
}

static void radio_mute_game_station(bool mute) {
    if (!g_game_radio_bus) return;

    if (mute && !g_radio_muted_by_us) {
        /* Guardar el volumen actual y silenciar */
        float *vol_ptr = (float *)((char *)g_game_radio_bus + BUS_OFF_VOLUME);
        g_original_radio_volume = *vol_ptr;
        radio_set_game_volume(0.0f);
        g_radio_muted_by_us = true;
        cabin_log("Radio control: muted game station (was %.2f)", g_original_radio_volume);
    } else if (!mute && g_radio_muted_by_us) {
        /* Restaurar el volumen */
        radio_set_game_volume(g_original_radio_volume);
        g_radio_muted_by_us = false;
        cabin_log("Radio control: restored game station volume to %.2f", g_original_radio_volume);
    }
}

#endif /* _WIN32 */

static uint64_t g_last_cfg_mtime = 0;
static unsigned int g_cfg_tick = 0;

static uint64_t file_mtime_token(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
           (uint64_t)fad.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return ((uint64_t)st.st_mtimespec.tv_sec << 32) ^
           (uint64_t)st.st_mtimespec.tv_nsec;
#endif
}

static void check_live_reload(void) {
    if ((g_cfg_tick++ % 30) != 0) return;
    char path[PATH_MAX];
    live_settings_path(path, sizeof(path));
    uint64_t token = file_mtime_token(path);
    if (token == 0) return;
    if (g_last_cfg_mtime == 0) {
        g_last_cfg_mtime = token;
        return;
    }
    if (token == g_last_cfg_mtime) return;
    g_last_cfg_mtime = token;

    g_settings = read_settings();
    if (g_eq_dsp) configure_eq(g_eq_dsp, g_settings);
    if (g_compressor_dsp) configure_compressor(g_compressor_dsp, g_settings);
    g_last_rot = 999.0f;
}

static void update_spatial_state(FMOD_STUDIO_SYSTEM *studio) {
    check_live_reload();
    if (!g_api.get_param) return;

    float raw_rot = read_studio_param(studio, "cabin_rot", 0.0f);
    float cabin_out = clamp_float(read_studio_param(studio, "cabin_out", 0.0f), 0.0f, 1.0f);
    float wnd_left = clamp_float(read_studio_param(studio, "wnd_left", 0.0f), 0.0f, 1.0f);
    float wnd_right = clamp_float(read_studio_param(studio, "wnd_right", 0.0f), 0.0f, 1.0f);
    float window_open = 0.5f * (wnd_left + wnd_right);

    /* Handle both radians [-pi, pi] and degrees [-180, 180] defensively. */
    float rot_rad = fabsf(raw_rot) > 3.25f ? (raw_rot * 0.0174532925f) : raw_rot;

    if (fabsf(rot_rad - g_last_rot) < 0.0015f &&
        fabsf(cabin_out - g_last_out) < 0.005f &&
        fabsf(wnd_left - g_last_wnd_l) < 0.005f &&
        fabsf(wnd_right - g_last_wnd_r) < 0.005f) {
        return;
    }

    g_last_rot = rot_rad;
    g_last_out = cabin_out;
    g_last_wnd_l = wnd_left;
    g_last_wnd_r = wnd_right;

    compute_spatial_targets(rot_rad, cabin_out, wnd_left, wnd_right);

    if (g_eq_dsp) {
        float open_air = clamp_float(cabin_out + 0.45f * window_open, 0.0f, 1.0f);
        float low_gain = g_settings.low_gain * (1.0f - 0.45f * open_air) - (2.2f * cabin_out);
        float mid_gain = g_settings.mid_gain - (1.5f * cabin_out);
        float high_gain = g_settings.high_gain - (3.2f * cabin_out);
        g_api.set_float(g_eq_dsp, EQ_LOWGAIN, low_gain);
        g_api.set_float(g_eq_dsp, EQ_MIDGAIN, mid_gain);
        g_api.set_float(g_eq_dsp, EQ_HIGHGAIN, high_gain);
    }

    if (!g_turn_logged && fabsf(rot_rad) > 0.25f) {
        g_turn_logged = true;
        cabin_log("360-degree 4.1 multi-speaker head-tracking confirmed (cabin_rot=%.2f rad).", rot_rad);
    }
}

static bool try_install(FMOD_STUDIO_SYSTEM *studio) {
    if (!resolve_symbols()) {
        g_disabled = true;
        cabin_log("Required FMOD symbols are unavailable; live processing disabled safely.");
        return false;
    }

    FMOD_RESULT result = FMOD_OK;
    if (!g_radio_bus) {
        result = g_api.get_bus(studio, "bus:/radio", &g_radio_bus);
        if (result != FMOD_OK || !g_radio_bus) {
            if (g_retry_counter == 30) {
                cabin_log("Waiting for bus:/radio (last FMOD result %d)...", result);
            }
            return false;
        }
        cabin_log("Found bus:/radio.");
    }

    FMOD_CHANNELGROUP *group = NULL;
    result = g_api.get_group(g_radio_bus, &group);
    if (result != FMOD_OK || !group) {
        if (!g_radio_bus_locked) {
            FMOD_RESULT lock_result = g_api.lock_bus(g_radio_bus);
            if (lock_result == FMOD_OK || lock_result == 78) {
                g_radio_bus_locked = true;
                if (lock_result == FMOD_OK) g_api.flush(studio);
            } else {
                cabin_log("Found radio bus but could not lock it (FMOD result %d)", lock_result);
                return false;
            }
        }
        result = g_api.get_group(g_radio_bus, &group);
        if (result != FMOD_OK || !group) {
            if (g_retry_counter == 30) {
                cabin_log("Radio bus channel group is not ready yet (FMOD result %d)", result);
            }
            return false;
        }
    }

#ifdef _WIN32
    g_radio_group = group;  /* guardar para el bridge */
#endif

    FMOD_SYSTEM *core = NULL;
    result = g_api.get_core(studio, &core);
    if (result != FMOD_OK || !core) {
        cabin_log("Could not access FMOD core system (FMOD result %d)", result);
        return false;
    }

    unsigned int version = 0;
    unsigned int build = 0;
    if (g_api.get_version && g_api.get_version(core, &version, &build) == FMOD_OK) {
        cabin_log("FMOD runtime version: 0x%08x (build %u)", version, build);
    }

    g_settings = read_settings();
    compute_spatial_targets(0.0f, 0.0f, 0.0f, 0.0f);
    int attached = 0;

    FMOD_DSP *eq = create_effect_by_type(core, FMOD_DSP_TYPE_THREE_EQ, "three-band cabin EQ");
    if (eq) {
        configure_eq(eq, g_settings);
        if (attach_effect(group, eq, "three-band cabin EQ")) {
            g_eq_dsp = eq;
            attached++;
        }
    }

    FMOD_DSP *spatial = create_spatial_cabin_dsp(core);
    if (spatial) {
        if (attach_effect(group, spatial, "3D 4.1 multi-speaker cabin spatializer (FL/FR/RL/RR + Sub + Reflections)")) {
            g_spatial_dsp = spatial;
            attached++;
        }
    }

    FMOD_DSP *compressor = create_effect_by_type(core, FMOD_DSP_TYPE_COMPRESSOR, "speaker compressor");
    if (compressor) {
        configure_compressor(compressor, g_settings);
        if (attach_effect(group, compressor, "speaker compressor")) {
            g_compressor_dsp = compressor;
            attached++;
        }
    }

    if (attached > 0) {
        cabin_log("Live 4.1 cabin processing active on bus:/radio (%d stages).", attached);
        float init_rot = 0.0f, init_final = 0.0f;
        if (g_api.get_param && g_api.get_param(studio, "cabin_rot", &init_rot, &init_final) == FMOD_OK) {
            cabin_log("Camera head-tracking active (cabin_rot=%.2f, front L/R + rear L/R + sub).", init_final);
            update_spatial_state(studio);
        }
#ifdef _WIN32
        /* Intentar conectar el bridge de audio externo */
        if (bridge_open()) {
            bridge_try_start(core);
        }
#endif
        return true;
    }

    cabin_log("No effects could be attached; original audio remains unchanged.");
    return false;
}

static void cabin_after_update(FMOD_STUDIO_SYSTEM *studio, const char *hook_name) {
    if (!g_hook_logged) {
        g_hook_logged = true;
        cabin_log("FMOD update hook active: %s", hook_name);
    }
    if (g_dry_run || g_disabled || !studio) return;

    if (g_installed) {
        update_spatial_state(studio);
#ifdef _WIN32
        /* Actualizar el bridge de audio externo */
        {
            FMOD_SYSTEM *core = NULL;
            if (g_api.get_core && g_api.get_core(studio, &core) == FMOD_OK && core) {
                bridge_update(core);
            }
        }
#endif
        return;
    }

    unsigned int tick = g_retry_counter++;
    if (tick < 3) return;
    unsigned int interval = tick < 35 ? 1 : 120;
    if ((tick % interval) != 0) return;

    lock_state();
    if (!g_installed && !g_disabled) g_installed = try_install(studio);
    unlock_state();
}

#ifdef __APPLE__

static FMOD_RESULT cabin_update(FMOD_STUDIO_SYSTEM *studio) {
    FMOD_RESULT result = FMOD_Studio_System_Update(studio);
    cabin_after_update(studio, "C API");
    return result;
}

static FMOD_RESULT cabin_cpp_update(FMOD_STUDIO_SYSTEM *studio) {
    FMOD_RESULT result = FMOD_CPP_Studio_System_Update(studio);
    cabin_after_update(studio, "C++ API");
    return result;
}

__attribute__((used))
static struct {
    const void *replacement;
    const void *replacee;
} update_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void *)cabin_update,
    (const void *)FMOD_Studio_System_Update
};

__attribute__((used))
static struct {
    const void *replacement;
    const void *replacee;
} cpp_update_interpose __attribute__((section("__DATA,__interpose"))) = {
    (const void *)cabin_cpp_update,
    (const void *)FMOD_CPP_Studio_System_Update
};

__attribute__((constructor))
static void cabin_loaded(void) {
    const char *dry_run = getenv("ETS2_CABIN_DRY_RUN");
    g_dry_run = dry_run && strcmp(dry_run, "1") == 0;
    cabin_log("ETS2 Cabin Radio live DSP loaded (pid %d).", getpid());
    if (g_dry_run) cabin_log("Dry-run mode enabled: no DSP will be attached.");
}

#elif defined(_WIN32)

static StudioUpdateFn g_orig_c_update = NULL;
static StudioUpdateFn g_orig_cpp_update = NULL;
static volatile LONG g_win_hook_started = 0;

static FMOD_RESULT cabin_win_c_update(FMOD_STUDIO_SYSTEM *studio) {
    FMOD_RESULT result = g_orig_c_update ? g_orig_c_update(studio) : FMOD_OK;
    cabin_after_update(studio, "Windows IAT (C API)");
    return result;
}

static FMOD_RESULT cabin_win_cpp_update(FMOD_STUDIO_SYSTEM *studio) {
    FMOD_RESULT result = g_orig_cpp_update ? g_orig_cpp_update(studio) : FMOD_OK;
    cabin_after_update(studio, "Windows IAT (C++ API)");
    return result;
}

static int patch_module_iat(HMODULE module) {
    if (!module) return 0;
    uint8_t *base = (uint8_t *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_DATA_DIRECTORY import_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!import_dir.VirtualAddress || !import_dir.Size) return 0;

    int patched = 0;
    IMAGE_IMPORT_DESCRIPTOR *desc =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + import_dir.VirtualAddress);

    for (; desc->Name != 0; desc++) {
        const char *dll_name = (const char *)(base + desc->Name);
        if (_strnicmp(dll_name, "fmodstudio", 10) != 0) continue;

        IMAGE_THUNK_DATA *orig_thunk = desc->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk)
            : NULL;
        IMAGE_THUNK_DATA *iat_thunk =
            (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);

        for (; iat_thunk->u1.Function != 0; iat_thunk++, orig_thunk = orig_thunk ? orig_thunk + 1 : NULL) {
            const char *sym_name = NULL;
            if (orig_thunk && !(orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                IMAGE_IMPORT_BY_NAME *by_name =
                    (IMAGE_IMPORT_BY_NAME *)(base + orig_thunk->u1.AddressOfData);
                sym_name = (const char *)by_name->Name;
            }

            ULONG_PTR current_fn = iat_thunk->u1.Function;
            ULONG_PTR replacement = 0;

            if ((g_orig_cpp_update && current_fn == (ULONG_PTR)g_orig_cpp_update) ||
                (sym_name && strcmp(sym_name, "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ") == 0)) {
                if (!g_orig_cpp_update) g_orig_cpp_update = (StudioUpdateFn)current_fn;
                replacement = (ULONG_PTR)&cabin_win_cpp_update;
            } else if ((g_orig_c_update && current_fn == (ULONG_PTR)g_orig_c_update) ||
                       (sym_name && strcmp(sym_name, "FMOD_Studio_System_Update") == 0)) {
                if (!g_orig_c_update) g_orig_c_update = (StudioUpdateFn)current_fn;
                replacement = (ULONG_PTR)&cabin_win_c_update;
            }

            if (replacement && current_fn != replacement) {
                DWORD old_protect = 0;
                if (VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old_protect)) {
                    iat_thunk->u1.Function = replacement;
                    VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), old_protect, &old_protect);
                    patched++;
                }
            }
        }
    }
    return patched;
}

static unsigned __stdcall cabin_hook_worker(void *unused) {
    (void)unused;
    for (int attempt = 0; attempt < 300; attempt++) {
        HMODULE fmod_studio = GetModuleHandleA("fmodstudio.dll");
        if (!fmod_studio) fmod_studio = GetModuleHandleA("fmodstudioL.dll");
        if (fmod_studio) {
            g_orig_cpp_update = (StudioUpdateFn)GetProcAddress(
                fmod_studio, "?update@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
            g_orig_c_update = (StudioUpdateFn)GetProcAddress(
                fmod_studio, "FMOD_Studio_System_Update");

            int patched = patch_module_iat(GetModuleHandleW(NULL));
            if (patched > 0) {
                cabin_log("Hooked FMOD Studio update via Windows PE IAT (%d slot%s).",
                          patched, patched == 1 ? "" : "s");
                return 0;
            }
        }
        Sleep(100);
    }
    cabin_log("Timed out waiting for fmodstudio.dll IAT in process.");
    return 0;
}

static void start_windows_hook_once(const char *source) {
    if (InterlockedCompareExchange(&g_win_hook_started, 1, 0) != 0) return;
    const char *dry_run = getenv("ETS2_CABIN_DRY_RUN");
    g_dry_run = dry_run && strcmp(dry_run, "1") == 0;
    cabin_log("ETS2 Cabin Radio live DSP loaded on Windows (%s, pid %lu).",
              source, (unsigned long)GetCurrentProcessId());
    if (g_dry_run) cabin_log("Dry-run mode enabled: no DSP will be attached.");

    uintptr_t thread = _beginthreadex(NULL, 0, cabin_hook_worker, NULL, 0, NULL);
    if (thread) CloseHandle((HANDLE)thread);
}

/* Optional SCS Telemetry SDK plugin entry points (for bin\win_x64\plugins\ auto-load). */
__declspec(dllexport) int __cdecl scs_telemetry_init(int version, const void *params) {
    (void)version;
    (void)params;
    start_windows_hook_once("SCS Plugin");
    return 0; /* SCS_RESULT_ok */
}

__declspec(dllexport) void __cdecl scs_telemetry_shutdown(void) {
    cabin_log("SCS plugin shutdown notification received.");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        start_windows_hook_once("DllMain");
    }
    return TRUE;
}

#endif
