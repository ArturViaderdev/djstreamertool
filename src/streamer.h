// Emisión en directo a uno o varios destinos RTMP a la vez (Twitch, YouTube...)
//
//  * Vídeo: un codificador H.264 por cada tasa de bits distinta (NVENC o x264).
//  * Audio: AAC a la frecuencia nativa de la entrada (44,1 o 48 kHz, ambas las
//    aceptan Twitch y YouTube), sin remuestrear ni corregir: el reloj de la
//    tarjeta de sonido manda y las marcas de tiempo del vídeo se ajustan a él.
//  * Cada destino tiene su conexión, cola e hilo: si uno se cae se reintenta
//    cada pocos segundos sin afectar al otro ni a la grabación FLAC.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct SwrContext;
class VideoEncoder;

struct StreamDest {
    std::string name;   // "Twitch", "YouTube"...
    std::string url;    // rtmp://servidor/app/clave o un fichero (.flv/.mkv) para pruebas
    int videoKbps = 4500;
};

struct StreamConfig {
    std::vector<StreamDest> dests;
    int width = 1280, height = 720, fps = 60;
    int keyintSec = 2;
    std::string encoder = "auto";   // auto | nvenc | x264
    std::string preset = "p5";
    int audioKbps = 160;
    int inputRate = 48000;          // frecuencia de la captura
    int avOffsetMs = 0;             // >0 retrasa el audio respecto al vídeo; <0 al revés
};

struct SinkStatus {
    std::string name, state;
    bool live = false;
    double kbps = 0;
    int reconnects = 0;
};

class Streamer {
public:
    Streamer();
    ~Streamer();
    bool start(const StreamConfig& c, std::string& err);
    void stop();   // puede tardar unos segundos (cierra las conexiones)
    bool active() const { return active_; }

    // Desde el hilo de la cámara / del audio
    void pushVideo(const uint8_t* bgra, int stride, int w, int h, int64_t tsUs);
    void pushAudio(const float* interleaved, int frames, int64_t captureUs);

    std::vector<SinkStatus> status();
    std::string encoderName() const { return encName_; }
    double encodeFps();
    uint64_t droppedFrames() const { return dropped_; }

    struct Sink;

private:
    void encodeLoop();
    void writerLoop(Sink* s);
    void distribute(AVPacket* p, bool video, int venc);
    void drainAudio();

    StreamConfig cfg_;
    std::atomic<bool> active_{false};
    uint64_t t0_ = 0;
    std::string encName_;

    // vídeo
    std::vector<std::unique_ptr<VideoEncoder>> encs_;
    std::vector<int> rates_;
    std::thread encThr_;
    std::mutex vm_;
    std::condition_variable vcv_;
    bool vactive_ = false, haveFrame_ = false;
    std::vector<uint8_t> pending_;
    int64_t pendingTs_ = 0;
    std::atomic<uint64_t> frames_{0}, dropped_{0};
    uint64_t fpsLastT_ = 0, fpsLastN_ = 0;
    double fps_ = 0;
    int64_t videoShiftUs_ = 0, audioShiftUs_ = 0;

    // audio
    std::mutex am_;
    bool aactive_ = false, aStarted_ = false;
    AVCodecContext* aenc_ = nullptr;
    AVCodecParameters* apar_ = nullptr;
    AVFrame* afr_ = nullptr;
    SwrContext* swr_ = nullptr;   // sólo si la entrada no es 44,1/48 kHz
    int outRate_ = 48000;
    int64_t a0Us_ = 0;
    int64_t inCount_ = 0, outCount_ = 0;
    std::vector<float> fifoL_, fifoR_;
    double drift_ = 0;
    int64_t nDrift_ = 0;
    std::atomic<int64_t> driftUs_{0};   // reloj del audio - reloj del sistema

    std::vector<std::unique_ptr<Sink>> sinks_;
    std::mutex stMtx_;   // status() frente a stop()
};
