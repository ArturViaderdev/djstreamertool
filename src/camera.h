// Captura de la webcam por V4L2 (YUYV o MJPEG), sin espejo, recortada al
// formato de salida y convertida a BGRA con libswscale.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct CamMode {
    uint32_t fourcc = 0;   // V4L2_PIX_FMT_MJPEG o V4L2_PIX_FMT_YUYV
    int w = 0, h = 0, fps = 0;
};

struct CamDevice {
    std::string path, name;
    std::vector<CamMode> modes;
};

std::vector<CamDevice> listCameras();
std::string fourccName(uint32_t f);

struct CameraParams {
    std::string device = "/dev/video0";
    uint32_t fourcc = 0;
    int w = 1280, h = 720, fps = 60;
    int outW = 1280, outH = 720;   // tamaño de salida (se recorta al centro si cambia la proporción)
};

class Camera {
public:
    // bgra: outW x outH, stride en bytes; tsUs: instante de captura (CLOCK_MONOTONIC)
    using FrameCb = std::function<void(const uint8_t* bgra, int w, int h, int stride, int64_t tsUs)>;

    ~Camera() { stop(); }
    void start(const CameraParams& p, FrameCb cb);
    void stop();
    bool running() const { return thr_.joinable(); }

    // Estado para la interfaz
    std::string status();
    double fps() const { return fps_.load(); }

private:
    void loop();
    bool openDev();
    void closeDev();
    void process(const uint8_t* data, size_t size, int64_t tsUs);
    void setStatus(const std::string& s);

    CameraParams p_;
    FrameCb cb_;
    std::thread thr_;
    std::atomic<bool> quit_{false};
    int fd_ = -1;
    bool mjpeg_ = false;
    int capW_ = 0, capH_ = 0, stride_ = 0;
    std::vector<std::pair<void*, size_t>> bufs_;
    std::vector<uint8_t> out_;
    struct Impl;
    Impl* impl_ = nullptr;

    std::mutex sm_;
    std::string status_;
    std::atomic<double> fps_{0};
    uint64_t fpsT0_ = 0;
    int fpsN_ = 0;
};
