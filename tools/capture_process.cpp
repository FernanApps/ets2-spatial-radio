// capture_process.cpp — Captura el audio de un proceso con Process Loopback.
//
// Dos modos:
//
//   1) MODO BRIDGE (dentro del juego):
//        capture_process.exe <PID> [volumen]
//      Escribe float32 estéreo 48 kHz en Local\ETS2CabinBridge (lo crea la DLL
//      de ETS2). Requiere que el juego esté corriendo con el plugin cargado.
//
//   2) MODO TEST (sin DLL, sin juego):
//        capture_process.exe <PID> --test [salida.wav] [--seconds N]
//      No toca el bridge. Graba el audio capturado a WAV 48 kHz / 16-bit
//      estéreo y muestra por consola paquetes/s, frames/s, RMS y pico.
//      Sirve para la prueba clave: mutear Chrome y ver si la captura sigue
//      recibiendo audio (frames/RMS) o si el mute la corta.
//
// Ejemplos:
//   python tools/audio_sessions.py                 (ver PID de Chrome)
//   capture_process.exe 12345 --test               (graba capture_test.wav)
//   capture_process.exe 12345 --test out.wav --seconds 30
//   python tools/mute_process.py chrome.exe 1      (Fase B: mutear)
//   python tools/mute_process.py chrome.exe 0      (desmutear)
//
// API: ActivateAudioInterfaceAsync + IAudioClient con
//      VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK (Windows 10 2004+).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cabin_bridge.h"

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Avrt.lib")

// ---------------------------------------------------------------------------
// Estado global mínimo
// ---------------------------------------------------------------------------

static volatile LONG g_running = 1;

static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_running, 0);
        return TRUE;
    }
    return FALSE;
}

static inline bool is_running(void) {
    return InterlockedCompareExchange(&g_running, 1, 1) != 0;
}

// ---------------------------------------------------------------------------
// Handler COM de finalización de ActivateAudioInterfaceAsync
// ---------------------------------------------------------------------------

class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivationHandler(HANDLE done_event)
        : m_ref(1), m_done_event(done_event), m_hr(E_PENDING), m_client(nullptr) {}

    ~ActivationHandler() {
        if (m_client) {
            m_client->Release();
            m_client = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)InterlockedIncrement(&m_ref);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = (ULONG)InterlockedDecrement(&m_ref);
        if (refs == 0) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation *operation) override {
        HRESULT hr_activate = E_UNEXPECTED;
        IUnknown *punk = nullptr;

        HRESULT hr = operation->GetActivateResult(&hr_activate, &punk);
        if (SUCCEEDED(hr)) hr = hr_activate;
        if (SUCCEEDED(hr) && punk) {
            hr = punk->QueryInterface(__uuidof(IAudioClient),
                                      reinterpret_cast<void **>(&m_client));
        }
        if (punk) punk->Release();

        m_hr = hr;
        SetEvent(m_done_event);
        return S_OK;
    }

    HRESULT result() const { return m_hr; }
    IAudioClient *client() const { return m_client; }

private:
    LONG m_ref;
    HANDLE m_done_event;
    HRESULT m_hr;
    IAudioClient *m_client;
};

// ---------------------------------------------------------------------------
// Escritura al ring buffer del bridge (mismo control de flujo que play_audio.py)
// ---------------------------------------------------------------------------

static void write_frames(CabinBridgeBlock *b, const float *src, uint32_t frames) {
    uint32_t done = 0;
    uint64_t last_read_change = GetTickCount64();
    int64_t last_read = b->read_pos;

    while (done < frames && is_running()) {
        int64_t write_pos = b->write_pos;
        int64_t read_pos = b->read_pos;

        // Si el consumidor lleva >1 s sin leer, resincronizamos en vez de
        // bloquear la captura para siempre (p. ej. DSP todavía sin enganchar).
        if (read_pos != last_read) {
            last_read = read_pos;
            last_read_change = GetTickCount64();
        } else if (GetTickCount64() - last_read_change > 1000) {
            b->write_pos = read_pos;
            last_read_change = GetTickCount64();
            continue;
        }

        int64_t buffered = write_pos - read_pos;
        int64_t room = (int64_t)CABIN_BRIDGE_TARGET_LATENCY - buffered;
        if (room <= 0) {
            Sleep(3);
            continue;
        }

        uint32_t chunk = frames - done;
        if ((int64_t)chunk > room) chunk = (uint32_t)room;

        const uint32_t data_len = CABIN_BRIDGE_CAPACITY * 2u;
        uint32_t start = (uint32_t)((write_pos & (CABIN_BRIDGE_CAPACITY - 1)) * 2);
        uint32_t total = chunk * 2u;
        uint32_t first = data_len - start;
        if (first > total) first = total;

        std::memcpy(&b->data[start], src + (size_t)done * 2,
                    (size_t)first * sizeof(float));
        if (total > first) {
            std::memcpy(&b->data[0], src + (size_t)done * 2 + first,
                        (size_t)(total - first) * sizeof(float));
        }

        MemoryBarrier();
        b->write_pos = write_pos + chunk;
        done += chunk;
    }
}

// ---------------------------------------------------------------------------
// WAV de prueba (PCM 16-bit, 48 kHz, estéreo)
// ---------------------------------------------------------------------------

struct WavWriter {
    FILE *fp = nullptr;
    uint64_t data_bytes = 0;
    bool failed = false;

    bool open(const std::wstring &path) {
        _wfopen_s(&fp, path.c_str(), L"wb");
        if (!fp) return false;

        uint8_t header[44] = {};
        auto put32 = [&](size_t off, uint32_t v) {
            header[off + 0] = (uint8_t)(v & 0xFF);
            header[off + 1] = (uint8_t)((v >> 8) & 0xFF);
            header[off + 2] = (uint8_t)((v >> 16) & 0xFF);
            header[off + 3] = (uint8_t)((v >> 24) & 0xFF);
        };
        auto put16 = [&](size_t off, uint16_t v) {
            header[off + 0] = (uint8_t)(v & 0xFF);
            header[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        };

        std::memcpy(header + 0, "RIFF", 4);
        put32(4, 36);  // se corrige al cerrar
        std::memcpy(header + 8, "WAVE", 4);
        std::memcpy(header + 12, "fmt ", 4);
        put32(16, 16);
        put16(20, 1);       // WAVE_FORMAT_PCM
        put16(22, 2);       // canales
        put32(24, 48000);   // sample rate
        put32(28, 48000 * 2 * 2);
        put16(32, 2 * 2);   // block align
        put16(34, 16);      // bits
        std::memcpy(header + 36, "data", 4);
        put32(40, 0);       // se corrige al cerrar

        if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
            failed = true;
            return false;
        }
        return true;
    }

    void write_float(const float *src, size_t sample_count) {
        if (!fp || failed || sample_count == 0) return;

        std::vector<int16_t> pcm(sample_count);
        for (size_t i = 0; i < sample_count; ++i) {
            float v = src[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            // -1.0 -> -32768, 1.0 -> 32767
            pcm[i] = (int16_t)lrintf(v * 32767.0f);
        }

        size_t wrote = fwrite(pcm.data(), sizeof(int16_t), pcm.size(), fp);
        data_bytes += (uint64_t)wrote * sizeof(int16_t);
        if (wrote != pcm.size()) failed = true;
    }

    void close() {
        if (!fp) return;

        uint32_t riff_size = (uint32_t)(36 + data_bytes);
        uint32_t data_size = (uint32_t)data_bytes;

        fseek(fp, 4, SEEK_SET);
        fwrite(&riff_size, 4, 1, fp);
        fseek(fp, 40, SEEK_SET);
        fwrite(&data_size, 4, 1, fp);
        fclose(fp);
        fp = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Medidor de nivel
// ---------------------------------------------------------------------------

struct LevelMeter {
    double sum_sq = 0.0;
    uint64_t samples = 0;
    float peak = 0.0f;

    void add(const float *src, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            float v = src[i];
            float a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            sum_sq += (double)v * (double)v;
        }
        samples += count;
    }

    double rms_db() const {
        if (samples == 0 || sum_sq <= 0.0) return -999.0;
        double rms = std::sqrt(sum_sq / (double)samples);
        return 20.0 * std::log10(rms);
    }

    void reset() {
        sum_sq = 0.0;
        samples = 0;
        peak = 0.0f;
    }
};

// ---------------------------------------------------------------------------
// Monitor en tiempo real por el dispositivo predeterminado (para pruebas)
// ---------------------------------------------------------------------------

class MonitorRenderer {
public:
    ~MonitorRenderer() { stop(); }

    bool start() {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator));
        if (FAILED(hr)) {
            std::printf("Monitor: CoCreateInstance falló (0x%08lX).\n",
                        (unsigned long)hr);
            return false;
        }

        hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &m_device);
        if (FAILED(hr)) {
            std::printf("Monitor: GetDefaultAudioEndpoint falló (0x%08lX).\n",
                        (unsigned long)hr);
            return false;
        }

        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(&m_client));
        if (FAILED(hr)) {
            std::printf("Monitor: Activate(IAudioClient) falló (0x%08lX).\n",
                        (unsigned long)hr);
            return false;
        }

        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = 48000;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = 8;
        fmt.nAvgBytesPerSec = 48000 * 8;
        fmt.cbSize = 0;

        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  0, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            std::printf("Monitor: Initialize falló (0x%08lX).\n",
                        (unsigned long)hr);
            return false;
        }

        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_event) {
            std::printf("Monitor: CreateEvent falló (%lu).\n",
                        (unsigned long)GetLastError());
            return false;
        }
        hr = m_client->SetEventHandle(m_event);
        if (FAILED(hr)) {
            std::printf("Monitor: SetEventHandle falló (0x%08lX).\n",
                        (unsigned long)hr);
            return false;
        }

        hr = m_client->GetBufferSize(&m_buffer_frames);
        if (FAILED(hr)) {
            std::printf("Monitor: GetBufferSize falló (0x%08lX).\n",
                        (unsigned long)hr);
            return false;
        }
        hr = m_client->GetService(IID_PPV_ARGS(&m_render));
        if (FAILED(hr)) {
            std::printf("Monitor: GetService(IAudioRenderClient) falló (0x%08lX).\n",
                        (unsigned long)hr);
            return false;
        }

        InitializeCriticalSection(&m_lock);
        m_lock_init = true;

        hr = m_client->Start();
        if (FAILED(hr)) {
            std::printf("Monitor: Start falló (0x%08lX).\n",
                        (unsigned long)hr);
            return false;
        }

        m_running = true;
        m_thread = CreateThread(nullptr, 0, &MonitorRenderer::thread_entry, this,
                                0, nullptr);
        if (!m_thread) return false;

        std::printf("Monitor: %.1f ms de buffer del dispositivo.\n",
                    (double)m_buffer_frames / 48000.0 * 1000.0);
        return true;
    }

    void push(const float *data, size_t count) {
        if (!m_running || count == 0) return;
        EnterCriticalSection(&m_lock);
        m_queue.insert(m_queue.end(), data, data + count);

        // Limitar la cola a ~200 ms: si el render no consume, no acumulamos
        // retraso infinito (se descartan las muestras más viejas).
        const size_t max_samples = 48000u * 2u / 5u;
        if (m_queue.size() > max_samples) {
            size_t excess = m_queue.size() - max_samples;
            m_queue.erase(m_queue.begin(), m_queue.begin() + excess);
            m_dropped += excess / 2;
        }
        LeaveCriticalSection(&m_lock);
    }

    double queued_ms() {
        if (!m_lock_init) return 0.0;
        EnterCriticalSection(&m_lock);
        size_t n = m_queue.size();
        LeaveCriticalSection(&m_lock);
        return (double)n / 2.0 / 48000.0 * 1000.0;
    }

    double buffer_ms() const {
        return (double)m_buffer_frames / 48000.0 * 1000.0;
    }

    void stop() {
        if (m_thread) {
            m_running = false;
            SetEvent(m_event);
            WaitForSingleObject(m_thread, 2000);
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
        if (m_client) m_client->Stop();
        if (m_render) {
            m_render->Release();
            m_render = nullptr;
        }
        if (m_client) {
            m_client->Release();
            m_client = nullptr;
        }
        if (m_event) {
            CloseHandle(m_event);
            m_event = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
        if (m_enumerator) {
            m_enumerator->Release();
            m_enumerator = nullptr;
        }
        if (m_lock_init) {
            DeleteCriticalSection(&m_lock);
            m_lock_init = false;
        }
    }

private:
    static DWORD WINAPI thread_entry(LPVOID self) {
        return static_cast<MonitorRenderer *>(self)->thread_proc();
    }

    DWORD thread_proc() {
        while (m_running) {
            DWORD w = WaitForSingleObject(m_event, 100);
            if (!m_running) break;
            if (w != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            if (FAILED(m_client->GetCurrentPadding(&padding))) continue;
            UINT32 avail = m_buffer_frames - padding;
            if (avail == 0) continue;

            BYTE *buffer = nullptr;
            if (FAILED(m_render->GetBuffer(avail, &buffer))) continue;

            float *out = reinterpret_cast<float *>(buffer);
            size_t needed = (size_t)avail * 2;
            EnterCriticalSection(&m_lock);
            size_t have = m_queue.size();
            size_t take = have < needed ? have : needed;
            for (size_t i = 0; i < take; ++i) out[i] = m_queue[i];
            if (take < needed) {
                std::memset(out + take, 0, (needed - take) * sizeof(float));
            }
            m_queue.erase(m_queue.begin(), m_queue.begin() + take);
            LeaveCriticalSection(&m_lock);

            m_render->ReleaseBuffer(avail, 0);
        }
        return 0;
    }

    IMMDeviceEnumerator *m_enumerator = nullptr;
    IMMDevice *m_device = nullptr;
    IAudioClient *m_client = nullptr;
    IAudioRenderClient *m_render = nullptr;
    HANDLE m_event = nullptr;
    HANDLE m_thread = nullptr;
    UINT32 m_buffer_frames = 0;
    bool m_running = false;
    bool m_lock_init = false;
    CRITICAL_SECTION m_lock;
    std::deque<float> m_queue;
    uint64_t m_dropped = 0;
};

// ---------------------------------------------------------------------------
// Diagnóstico
// ---------------------------------------------------------------------------

static void print_hr(const char *what, HRESULT hr) {
    LPWSTR msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                       FORMAT_MESSAGE_ALLOCATE_BUFFER,
                   nullptr, (DWORD)hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPWSTR)&msg, 0, nullptr);
    std::printf("ERROR: %s (0x%08lX)\n", what, (unsigned long)hr);
    if (msg) {
        std::printf("       %ls\n", msg);
        LocalFree(msg);
    }
}

static HRESULT initialize_capture_client(IAudioClient *client, bool use_float,
                                         WAVEFORMATEX *out_format) {
    WAVEFORMATEX fmt = {};
    fmt.wFormatTag = use_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = use_float ? 32 : 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize = 0;

    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &fmt,
                                    nullptr);
    if (SUCCEEDED(hr) && out_format) *out_format = fmt;
    return hr;
}

// Ganancia segura para un paquete ya convertido a float.
//
// - Si el pico supera el umbral esperado del duck, devuelve 0 (silencio):
//   significa que el volumen subió a mano o el duck no está aplicado.
// - Si no, aplica la ganancia pedida pero con limitador: el pico de salida
//   nunca supera 0.95, así que un cambio de volumen no puede reventar el audio.
static float safe_boost_gain(const float *data, size_t count, float requested,
                             float gate_peak, bool *gated) {
    *gated = false;

    float peak = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        float a = std::fabs(data[i]);
        if (a > peak) peak = a;
    }

    if (gate_peak > 0.0f && peak > gate_peak) {
        *gated = true;
        return 0.0f;
    }

    if (requested <= 1.0f) return requested;
    if (peak <= 0.0f) return requested;

    float limit_gain = 0.95f / peak;
    return requested < limit_gain ? requested : limit_gain;
}

// ---------------------------------------------------------------------------
// Ducking del volumen de sesión (truco para mutear sin perder la captura)
//
// El process loopback se toma DESPUÉS del volumen de sesión y ANTES del mute
// del endpoint. Por eso bajamos la sesión a ~1e-5 (−100 dB, inaudible) y
// recuperamos el nivel con --boost. Al salir se restaura el volumen original.
// ---------------------------------------------------------------------------

struct DuckedSession {
    ISimpleAudioVolume *volume = nullptr;
    float original_volume = 1.0f;
    BOOL original_mute = FALSE;
    DWORD pid = 0;
};

static void collect_process_tree(DWORD root, std::set<DWORD> &tree) {
    tree.insert(root);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    std::vector<std::pair<DWORD, DWORD>> procs;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            procs.emplace_back(pe.th32ProcessID, pe.th32ParentProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &p : procs) {
            if (tree.count(p.second) && !tree.count(p.first)) {
                tree.insert(p.first);
                changed = true;
            }
        }
    }
}

// Devuelve cuántas sesiones nuevas se han duckeado.
static int duck_target_sessions(
    DWORD target_pid, float duck_volume,
    std::map<std::wstring, DuckedSession> &saved) {
    std::set<DWORD> tree;
    collect_process_tree(target_pid, tree);

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return 0;

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        enumerator->Release();
        return 0;
    }

    UINT endpoint_count = 0;
    collection->GetCount(&endpoint_count);
    int newly_ducked = 0;

    for (UINT e = 0; e < endpoint_count; ++e) {
        IMMDevice *device = nullptr;
        if (FAILED(collection->Item(e, &device))) continue;

        IAudioSessionManager2 *manager = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                    nullptr, reinterpret_cast<void **>(&manager)))) {
            device->Release();
            continue;
        }

        IAudioSessionEnumerator *sessions = nullptr;
        if (FAILED(manager->GetSessionEnumerator(&sessions))) {
            manager->Release();
            device->Release();
            continue;
        }

        int session_count = 0;
        sessions->GetCount(&session_count);
        for (int s = 0; s < session_count; ++s) {
            IAudioSessionControl *control = nullptr;
            if (FAILED(sessions->GetSession(s, &control))) continue;

            IAudioSessionControl2 *control2 = nullptr;
            if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&control2)))) {
                DWORD pid = 0;
                if (SUCCEEDED(control2->GetProcessId(&pid)) && tree.count(pid)) {
                    LPWSTR instance_id = nullptr;
                    if (SUCCEEDED(control2->GetSessionInstanceIdentifier(
                            &instance_id)) &&
                        instance_id) {
                        std::wstring key(instance_id);
                        CoTaskMemFree(instance_id);

                        auto it = saved.find(key);
                        if (it == saved.end()) {
                            ISimpleAudioVolume *volume = nullptr;
                            if (SUCCEEDED(control->QueryInterface(
                                    IID_PPV_ARGS(&volume)))) {
                                DuckedSession ds;
                                ds.volume = volume;
                                ds.pid = pid;
                                volume->GetMasterVolume(&ds.original_volume);
                                volume->GetMute(&ds.original_mute);
                                volume->SetMasterVolume(duck_volume, nullptr);
                                volume->SetMute(FALSE, nullptr);
                                saved.emplace(key, ds);
                                ++newly_ducked;
                            }
                        } else {
                            // Re-aplicar: si el usuario (u otra app) subió el
                            // volumen mientras capturábamos, lo volvemos a bajar.
                            it->second.volume->SetMasterVolume(duck_volume, nullptr);
                            it->second.volume->SetMute(FALSE, nullptr);
                        }
                    }
                }
                control2->Release();
            }
            control->Release();
        }

        sessions->Release();
        manager->Release();
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    return newly_ducked;
}

static void restore_ducked_sessions(
    std::map<std::wstring, DuckedSession> &saved) {
    for (auto &kv : saved) {
        DuckedSession &ds = kv.second;
        if (ds.volume) {
            ds.volume->SetMasterVolume(ds.original_volume, nullptr);
            ds.volume->SetMute(ds.original_mute, nullptr);
            ds.volume->Release();
            ds.volume = nullptr;
        }
    }
    saved.clear();
}

// ---------------------------------------------------------------------------
// Opciones de línea de comandos
// ---------------------------------------------------------------------------

struct Options {
    DWORD pid = 0;
    float gain = 1.0f;
    bool test_mode = false;
    bool have_wav = false;
    bool force_float = false;
    bool monitor = false;
    bool duck = false;
    float duck_volume = 0.00001f;  // −100 dB: inaudible en altavoces
    double boost_db = 0.0;
    std::wstring wav_path;
    DWORD seconds = 0;  // 0 = hasta Ctrl+C
};

static void usage(void) {
    std::printf(
        "Uso:\n"
        "  capture_process.exe <PID> [volumen]                  (bridge del juego)\n"
        "  capture_process.exe <PID> --test [salida.wav] [--seconds N]\n"
        "\n"
        "  --test          Modo autónomo: no usa el bridge, graba WAV y muestra\n"
        "                  paquetes/s, frames/s, RMS y pico. Sin ETS2 ni DLL.\n"
        "  --seconds N     Detener automáticamente tras N segundos.\n"
        "  --float         Capturar en float32 (recomendado con --boost).\n"
        "  --boost DB      Amplificar la captura DB decibelios (compensa el volumen\n"
        "                  bajo de la sesión; p. ej. --boost 60 para volumen 0.001).\n"
        "  --duck          Baja el volumen de las sesiones del proceso a ~0.00001\n"
        "                  (inaudible), activa float32 y compensa el boost solo.\n"
        "                  Restaura el volumen original al salir.\n"
        "  --duck-volume V Volumen al que se deja la app (por defecto 0.00001).\n"
        "  --monitor       Reproduce la captura EN TIEMPO REAL por el dispositivo\n"
        "                  predeterminado, sin bridge. Implica --duck. Sirve para\n"
        "                  probar continuidad y latencia antes de usarlo en ETS2.\n"
        "  volumen         Ganancia 0.0-2.0 (solo modo bridge; por defecto 1.0).\n"
        "\n"
        "Utilidades:\n"
        "  python tools/audio_sessions.py             lista PID/proceso/mute\n"
        "  python tools/mute_process.py chrome.exe 1  mutea Chrome\n"
        "  python tools/mute_process.py chrome.exe 0  desmutea Chrome\n");
}

static bool parse_options(int argc, wchar_t **argv, Options *opt) {
    if (argc < 2) return false;

    wchar_t *end = nullptr;
    unsigned long pid_value = wcstoul(argv[1], &end, 0);
    if (!end || *end != L'\0' || pid_value == 0 || pid_value > 0xFFFFFFFFul) {
        std::printf("ERROR: PID inválido '%ls'.\n", argv[1]);
        return false;
    }
    opt->pid = (DWORD)pid_value;

    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];

        if (arg == L"--test") {
            opt->test_mode = true;
            // Ruta WAV opcional justo después de --test: --test salida.wav
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                opt->have_wav = true;
                opt->wav_path = argv[++i];
            }
        } else if (arg == L"--seconds") {
            if (i + 1 >= argc) {
                std::printf("ERROR: --seconds requiere un número.\n");
                return false;
            }
            opt->seconds = (DWORD)wcstoul(argv[++i], nullptr, 0);
        } else if (arg == L"--wav") {
            if (i + 1 >= argc) {
                std::printf("ERROR: --wav requiere una ruta.\n");
                return false;
            }
            opt->have_wav = true;
            opt->wav_path = argv[++i];
        } else if (arg == L"--float") {
            opt->force_float = true;
        } else if (arg == L"--duck") {
            opt->duck = true;
        } else if (arg == L"--monitor") {
            opt->monitor = true;
            opt->duck = true;  // sin duck habría doble audio (Chrome + monitor)
        } else if (arg == L"--duck-volume") {
            if (i + 1 >= argc) {
                std::printf("ERROR: --duck-volume requiere un valor.\n");
                return false;
            }
            end = nullptr;
            double v = wcstod(argv[++i], &end);
            if (!end || *end != L'\0' || v <= 0.0 || v > 1.0) {
                std::printf("ERROR: --duck-volume fuera de rango (0 < v <= 1).\n");
                return false;
            }
            opt->duck_volume = (float)v;
        } else if (arg == L"--boost") {
            if (i + 1 >= argc) {
                std::printf("ERROR: --boost requiere decibelios (p. ej. 60).\n");
                return false;
            }
            end = nullptr;
            double db = wcstod(argv[++i], &end);
            if (!end || *end != L'\0' || db < 0.0 || db > 120.0) {
                std::printf("ERROR: --boost fuera de rango (0-120 dB).\n");
                return false;
            }
            opt->boost_db = db;
        } else if (arg == L"--help" || arg == L"-h") {
            return false;
        } else {
            // Argumento numérico suelto = volumen (compatibilidad con el doc).
            end = nullptr;
            double v = wcstod(arg.c_str(), &end);
            if (!end || *end != L'\0' || v < 0.0 || v > 2.0) {
                std::printf("ERROR: argumento no reconocido '%ls'.\n",
                            arg.c_str());
                return false;
            }
            opt->gain = (float)v;
        }
    }

    if (opt->test_mode && !opt->have_wav) {
        opt->have_wav = true;
        opt->wav_path = L"capture_test.wav";
    }

    // Con ducking hace falta float32: a −100 dB el int16 no tiene resolución.
    if (opt->duck) opt->force_float = true;

    return true;
}

// ---------------------------------------------------------------------------
// Bucle principal
// ---------------------------------------------------------------------------

static int run(const Options &opt) {
    int rc = 1;
    HRESULT hr = E_FAIL;

    HANDLE map_handle = nullptr;
    CabinBridgeBlock *bridge = nullptr;

    HANDLE activated_event = nullptr;
    HANDLE sample_event = nullptr;
    ActivationHandler *handler = nullptr;
    IActivateAudioInterfaceAsyncOperation *async_op = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    bool started = false;

    WavWriter wav;
    LevelMeter meter;
    MonitorRenderer monitor;
    std::vector<float> scratch;
    std::map<std::wstring, DuckedSession> ducked;
    uint64_t total_frames = 0;
    uint64_t total_packets = 0;
    uint64_t silent_packets = 0;
    uint64_t gated_packets = 0;
    double boost_db = opt.boost_db;

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        print_hr("CoInitializeEx", hr);
        return 1;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    // Ducking opcional: bajar la sesión a un volumen inaudible y compensar.
    if (opt.duck) {
        if (boost_db <= 0.0) {
            // 1.5 dB de margen para no recortar los picos del audio original.
            boost_db = -20.0 * std::log10((double)opt.duck_volume) - 1.5;
        }
        int n = duck_target_sessions(opt.pid, opt.duck_volume, ducked);
        std::printf("Ducking: %d sesión(es) a volumen %.6g (%.0f dB por debajo); "
                    "boost +%.0f dB.\n",
                    n, (double)opt.duck_volume,
                    20.0 * std::log10(1.0 / (double)opt.duck_volume), boost_db);
        std::printf("El volumen original se restaura al salir.\n");
    }

    // 1. Bridge (solo si no estamos en modo test ni monitor).
    if (!opt.test_mode && !opt.monitor) {
        map_handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                      L"Local\\ETS2CabinBridge");
        if (!map_handle) {
            std::printf("ERROR: no se pudo abrir Local\\ETS2CabinBridge (error %lu).\n",
                        (unsigned long)GetLastError());
            std::printf("       Inicia ETS2 con ETS2CabinDSP.dll cargado,\n");
            std::printf("       o usa --test para probar sin juego/DLL.\n");
            goto cleanup;
        }

        bridge = (CabinBridgeBlock *)MapViewOfFile(
            map_handle, FILE_MAP_ALL_ACCESS, 0, 0, CABIN_BRIDGE_SIZE);
        if (!bridge) {
            std::printf("ERROR: MapViewOfFile falló (error %lu).\n",
                        (unsigned long)GetLastError());
            goto cleanup;
        }

        int64_t read_pos = bridge->read_pos;
        bridge->sample_rate = 48000;
        bridge->channels = 2;
        bridge->gain = opt.gain;
        bridge->source_pid = opt.pid;
        bridge->write_pos = read_pos;
        bridge->capturing = 1;
        bridge->enabled = 1;
        MemoryBarrier();
        bridge->magic = CABIN_BRIDGE_MAGIC;
        bridge->version = CABIN_BRIDGE_VERSION;
    }

    // 2. WAV opcional.
    if (opt.have_wav) {
        if (!wav.open(opt.wav_path)) {
            std::printf("ERROR: no se pudo crear el WAV de salida '%ls'.\n",
                        opt.wav_path.c_str());
            goto cleanup;
        }
    }

    // 3. Activar process loopback para el PID.
    activated_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sample_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!activated_event || !sample_event) {
        std::printf("ERROR: CreateEvent falló (error %lu).\n",
                    (unsigned long)GetLastError());
        goto cleanup;
    }

    {
        AUDIOCLIENT_ACTIVATION_PARAMS params = {};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = opt.pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT pv = {};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = (BYTE *)&params;

        handler = new (std::nothrow) ActivationHandler(activated_event);
        if (!handler) {
            std::printf("ERROR: sin memoria para el handler COM.\n");
            goto cleanup;
        }

        hr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
            handler, &async_op);
        if (FAILED(hr)) {
            print_hr("ActivateAudioInterfaceAsync", hr);
            std::printf("       PID objetivo: %lu\n", (unsigned long)opt.pid);
            std::printf("       ¿El proceso existe y está reproduciendo audio?\n");
            goto cleanup;
        }
    }

    if (WaitForSingleObject(activated_event, 10000) != WAIT_OBJECT_0) {
        std::printf("ERROR: la activación no terminó en 10 s.\n");
        goto cleanup;
    }

    hr = handler->result();
    if (FAILED(hr)) {
        print_hr("activación de IAudioClient", hr);
        if (hr == E_INVALIDARG) {
            std::printf("       Comprueba que el PID sea el de la sesión de audio.\n");
        }
        goto cleanup;
    }

    client = handler->client();
    if (!client) {
        std::printf("ERROR: activación sin IAudioClient.\n");
        goto cleanup;
    }

    // 4. Initialize: int16 primero (formato del sample oficial); float32 como
    //    alternativa. AUTOCONVERTPCM resamplea a 48 kHz estéreo.
    bool capture_is_float = false;
    {
        WAVEFORMATEX chosen = {};
        HRESULT hr_init;

        // Con --boost conviene float32: a −60/−80 dBFS el int16 se queda sin
        // resolución y el boost amplificaría el ruido de cuantización.
        if (opt.force_float) {
            hr_init = initialize_capture_client(client, true, &chosen);
            capture_is_float = true;
            if (FAILED(hr_init)) {
                std::printf("Aviso: Initialize float32 falló (0x%08lX); probando int16.\n",
                            (unsigned long)hr_init);
                hr_init = initialize_capture_client(client, false, &chosen);
                capture_is_float = false;
            }
        } else {
            hr_init = initialize_capture_client(client, false, &chosen);
            if (FAILED(hr_init)) {
                std::printf("Aviso: Initialize int16 falló (0x%08lX); probando float32.\n",
                            (unsigned long)hr_init);
                hr_init = initialize_capture_client(client, true, &chosen);
                capture_is_float = true;
            }
        }

        if (FAILED(hr_init)) {
            print_hr("IAudioClient::Initialize", hr_init);
            goto cleanup;
        }
        std::printf("Formato: %s, %u Hz, %u canales\n",
                    capture_is_float ? "float32" : "int16",
                    (unsigned)chosen.nSamplesPerSec, (unsigned)chosen.nChannels);
    }

    const float boost =
        boost_db > 0.0 ? (float)std::pow(10.0, boost_db / 20.0) : 1.0f;
    if (boost_db > 0.0) {
        std::printf("Boost de compensación: %.1f dB (x%.0f)\n", boost_db,
                    (double)boost);
    }

    // 5. Servicio de captura + evento + Start.
    hr = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(hr)) {
        print_hr("GetService(IAudioCaptureClient)", hr);
        goto cleanup;
    }

    hr = client->SetEventHandle(sample_event);
    if (FAILED(hr)) {
        print_hr("SetEventHandle", hr);
        goto cleanup;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        print_hr("IAudioClient::Start", hr);
        goto cleanup;
    }
    started = true;

    DWORD mmcss_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &mmcss_index);

    if (opt.monitor) {
        if (!monitor.start()) {
            std::printf("ERROR: no se pudo abrir el dispositivo de salida para --monitor.\n");
            goto cleanup;
        }
        std::printf("\nMODO MONITOR EN TIEMPO REAL — sin bridge ni DLL.\n");
        std::printf("PID %lu. Chrome queda a volumen ~0 y lo que capturamos se\n",
                    (unsigned long)opt.pid);
        std::printf("reproduce por tu dispositivo predeterminado.\n");
        std::printf("Cambia de canción/vídeo cuando quieras: el stream sigue.\n");
        std::printf("Ctrl+C para detener%s.\n\n",
                    opt.seconds ? " (o espera --seconds)" : "");
    } else if (opt.test_mode) {
        std::printf("\nMODO TEST — sin bridge, sin DLL.\n");
        std::printf("PID %lu. Grabando a: %ls\n", (unsigned long)opt.pid,
                    opt.wav_path.c_str());
        std::printf("Prueba: reproduce audio, y luego mutea/desmutea el proceso.\n");
        std::printf("Si tras mutear siguen subiendo 'pkt' y 'frames', la captura\n");
        std::printf("sobrevive al mute. Si se congelan o el RMS cae a -inf, no.\n");
        std::printf("Ctrl+C para detener%s.\n\n",
                    opt.seconds ? " (o espera --seconds)" : "");
    } else {
        std::printf("\nMODO BRIDGE — PID %lu, ganancia %.2f.\n",
                    (unsigned long)opt.pid, opt.gain);
        std::printf("Ctrl+C para detener.\n\n");
    }

    uint64_t start_tick = GetTickCount64();
    uint64_t last_status = start_tick;
    uint64_t last_duck = start_tick;
    uint64_t last_gated = 0;
    uint64_t sec_packets = 0;
    uint64_t sec_frames = 0;

    // 6. Bucle de captura.
    bool force_duck = false;
    while (is_running()) {
        // Si el gate detectó un pico, re-aplicar el duck YA (no esperar al
        // tick de 1 s). Así el corte por seguridad dura milisegundos.
        if (force_duck) {
            duck_target_sessions(opt.pid, opt.duck_volume, ducked);
            last_duck = GetTickCount64();
            force_duck = false;
        }

        if (opt.seconds) {
            DWORD elapsed = (DWORD)((GetTickCount64() - start_tick) / 1000);
            if (elapsed >= opt.seconds) break;
        }

        DWORD wait = WaitForSingleObject(sample_event, 200);
        if (wait == WAIT_FAILED) {
            std::printf("ERROR: WaitForSingleObject falló (error %lu).\n",
                        (unsigned long)GetLastError());
            break;
        }
        if (wait == WAIT_TIMEOUT) {
            // Sin paquetes: seguimos para poder imprimir estado igualmente.
        } else {
            UINT32 packet_frames = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet_frames)) &&
                   packet_frames > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 device_pos = 0;
                UINT64 qpc_pos = 0;

                HRESULT hr_buf = capture->GetBuffer(&data, &frames, &flags,
                                                    &device_pos, &qpc_pos);
                if (FAILED(hr_buf)) break;

                if (frames > 0) {
                    scratch.resize((size_t)frames * 2);

                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        std::memset(scratch.data(), 0,
                                    scratch.size() * sizeof(float));
                        ++silent_packets;
                    } else if (capture_is_float) {
                        std::memcpy(scratch.data(), data,
                                    scratch.size() * sizeof(float));
                    } else {
                        const int16_t *src =
                            reinterpret_cast<const int16_t *>(data);
                        for (size_t i = 0; i < scratch.size(); ++i) {
                            scratch[i] = (float)src[i] / 32768.0f;
                        }
                    }

                    // Compensar el volumen bajo de la sesión con protección.
                    // Si el duck no está aplicado (pico inesperadamente alto),
                    // se silencia en vez de amplificar: evita reventar el audio.
                    if (boost != 1.0f) {
                        const float gate_peak =
                            opt.duck ? opt.duck_volume * 8.0f : 0.0f;
                        bool gated = false;
                        float packet_gain = safe_boost_gain(
                            scratch.data(), scratch.size(), boost, gate_peak,
                            &gated);
                        if (gated) {
                            std::memset(scratch.data(), 0,
                                        scratch.size() * sizeof(float));
                            ++gated_packets;
                            if (opt.duck) force_duck = true;
                        } else if (packet_gain != 1.0f) {
                            for (size_t i = 0; i < scratch.size(); ++i) {
                                scratch[i] *= packet_gain;
                            }
                        }
                    }

                    // Liberar WASAPI antes del control de flujo: write_frames
                    // puede dormir y no queremos retener el buffer del motor.
                    capture->ReleaseBuffer(frames);

                    meter.add(scratch.data(), scratch.size());
                    // El monitor va antes del bridge: write_frames puede dormir
                    // por control de flujo y no queremos retrasar la reproducción.
                    if (opt.monitor) monitor.push(scratch.data(), scratch.size());
                    if (bridge) write_frames(bridge, scratch.data(), frames);
                    if (wav.fp) wav.write_float(scratch.data(), scratch.size());

                    total_frames += frames;
                    total_packets += 1;
                    sec_frames += frames;
                    sec_packets += 1;
                } else {
                    capture->ReleaseBuffer(frames);
                }
            }
        }

        uint64_t now = GetTickCount64();

        // Si Chrome crea una sesión nueva (otra pestaña, otro dispositivo),
        // duckearla también para que no se escape por los altavoces.
        if (opt.duck && now - last_duck >= 1000) {
            int n = duck_target_sessions(opt.pid, opt.duck_volume, ducked);
            if (n > 0) std::printf("Ducking: %d sesión(es) nuevas.\n", n);
            last_duck = now;
        }

        if (now - last_status >= 1000) {
            double rms = meter.rms_db();
            if (opt.test_mode) {
                if (rms < -900.0) {
                    std::printf(
                        "  [%3llus] pkt/s: %4llu  frames/s: %6llu  RMS:  -inf  pico: %.3f  pkt_total: %llu\n",
                        (unsigned long long)((now - start_tick) / 1000),
                        (unsigned long long)sec_packets,
                        (unsigned long long)sec_frames, (double)meter.peak,
                        (unsigned long long)total_packets);
                } else {
                    std::printf(
                        "  [%3llus] pkt/s: %4llu  frames/s: %6llu  RMS: %6.1f dBFS  pico: %.3f  pkt_total: %llu\n",
                        (unsigned long long)((now - start_tick) / 1000),
                        (unsigned long long)sec_packets,
                        (unsigned long long)sec_frames, rms,
                        (double)meter.peak, (unsigned long long)total_packets);
                }
            } else {
                int64_t buffered = bridge ? (bridge->write_pos - bridge->read_pos) : 0;
                std::printf(
                    "  [%3llus] frames: %llu  buf: %lld  playing: %u  RMS: %.1f dBFS\n",
                    (unsigned long long)((now - start_tick) / 1000),
                    (unsigned long long)total_frames, (long long)buffered,
                    bridge ? (unsigned)bridge->playing : 0u,
                    meter.rms_db() < -900.0 ? -999.0 : meter.rms_db());
            }

            if (opt.monitor) {
                double q = monitor.queued_ms();
                double b = monitor.buffer_ms();
                std::printf(
                    "           monitor: cola %.1f ms + buffer %.1f ms ≈ %.1f ms (más el buffer de captura)\n",
                    q, b, q + b);
            }

            if (gated_packets > last_gated) {
                std::printf(
                    "  aviso: %llu paquete(s) silenciados por pico inesperado (¿volumen subido a mano?)\n",
                    (unsigned long long)(gated_packets - last_gated));
                last_gated = gated_packets;
            }

            meter.reset();
            sec_packets = 0;
            sec_frames = 0;
            last_status = now;
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);

    std::printf("\nDetenido. Paquetes: %llu  frames: %llu  silenciosos: %llu\n",
                (unsigned long long)total_packets,
                (unsigned long long)total_frames,
                (unsigned long long)silent_packets);
    if (opt.have_wav) {
        std::printf("WAV: %ls (%.1f s)\n", opt.wav_path.c_str(),
                    (double)total_frames / 48000.0);
    }
    rc = 0;

cleanup:
    if (started && client) client->Stop();
    if (capture) capture->Release();
    if (handler) handler->Release();  // libera también el IAudioClient
    if (async_op) async_op->Release();
    if (sample_event) CloseHandle(sample_event);
    if (activated_event) CloseHandle(activated_event);

    if (wav.fp) wav.close();

    // Restaurar el volumen original de la app antes de salir.
    if (opt.duck) restore_ducked_sessions(ducked);

    if (bridge) {
        bridge->capturing = 0;
        bridge->enabled = 0;
        MemoryBarrier();
        UnmapViewOfFile(bridge);
        bridge = nullptr;
    }
    if (map_handle) CloseHandle(map_handle);

    monitor.stop();
    CoUninitialize();
    return rc;
}

// ---------------------------------------------------------------------------
// Entrada
// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t **argv) {
    Options opt;
    if (!parse_options(argc, argv, &opt)) {
        usage();
        return 2;
    }
    return run(opt);
}
