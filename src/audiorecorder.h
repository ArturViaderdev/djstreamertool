// Grabación del audio de la sesión en FLAC o MP3, independiente del directo:
// si se cae la conexión la grabación sigue. Cada grabador codifica en su propio
// hilo para que el disco nunca frene la captura; se pueden usar varios a la vez
// (FLAC + MP3 de la misma sesión).
//
// FLAC: por defecto a la frecuencia de la entrada (sin remuestrear = copia
// exacta de lo que entrega la tarjeta). Si se pide otra (44,1 kHz para CD...)
// se usa el remuestreador de alta calidad (SoX si está).
// MP3: LAME, en CBR (320/256/192 kbps) o VBR V0. MP3 sólo admite hasta 48 kHz.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RecordOptions {
    enum Format { FLAC, MP3 } format = FLAC;
    int inRate = 48000;
    int outRate = 0;     // 0 = la de la entrada
    int bits = 16;       // FLAC: 16 o 24
    int inBits = 32;     // resolución real de la fuente (FLAC 16 bits sin dither si ya es 16)
    int mp3Kbps = 320;   // MP3: 0 = VBR V0
};

class AudioRecorder {
public:
    ~AudioRecorder() { stop(); }

    bool start(const std::string& path, const RecordOptions& o, std::string& err);
    void push(const float* interleaved, int frames);   // desde el hilo del audio
    void stop();

    bool active() const { return active_; }
    std::string path() const { return path_; }
    uint64_t bytes() const { return bytes_; }
    double seconds() const { return outRate_ ? (double)samples_ / outRate_ : 0; }
    std::string error();
    const char* formatName() const { return opt_.format == RecordOptions::MP3 ? "MP3" : "FLAC"; }

private:
    void loop();
    bool writeFrames(bool flush);

    RecordOptions opt_;
    std::string path_;
    int outRate_ = 0;
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> bytes_{0}, samples_{0};
    std::thread thr_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::vector<float>> q_;
    bool stop_ = false;
    std::mutex em_;
    std::string err_;

    struct Impl;
    Impl* impl_ = nullptr;
};
