// Captura de la entrada de línea por PulseAudio / PipeWire (pipewire-pulse).
// Se captura a la frecuencia nativa de la fuente para no remuestrear nada: el
// volumen que pongas en KDE para esa entrada se aplica aquí.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioSourceInfo {
    std::string name;          // nombre interno (pactl)
    std::string description;   // nombre legible
    std::string format;        // s16le, s24le, s32le, float32le...
    int rate = 0, channels = 0;
    int bits() const;          // resolución real de la fuente (16, 24, 32)
};

std::vector<AudioSourceInfo> listAudioSources();   // sin los "monitor" de las salidas
std::string defaultAudioSource();

class AudioCapture {
public:
    // interleaved: estéreo float; captureUs: instante (CLOCK_MONOTONIC) de la 1ª muestra
    using BlockCb = std::function<void(const float* interleaved, int frames, int64_t captureUs)>;

    ~AudioCapture() { stop(); }
    void start(const std::string& source, int rate, BlockCb cb);
    void stop();
    int rate() const { return rate_; }
    std::string status();

    // Picos (lineales) desde la última llamada y si ha habido saturación
    void takeLevels(float peak[2], bool clip[2]);

private:
    void loop();
    void setStatus(const std::string& s);

    std::string source_;
    int rate_ = 48000;
    BlockCb cb_;
    std::thread thr_;
    std::atomic<bool> quit_{false};

    std::mutex lm_;
    float peak_[2] = {0, 0};
    bool clip_[2] = {false, false};

    std::mutex sm_;
    std::string status_;
};
