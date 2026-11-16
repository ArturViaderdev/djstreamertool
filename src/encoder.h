// Codificadores de vídeo H.264: NVENC directo (reaprovechado de gamecapture) o
// x264 por software como alternativa si no hay GPU NVIDIA.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct VideoEncoderParams {
    int w = 1280, h = 720, fps = 60;
    int kbps = 4500;
    int keyintSec = 2;
    std::string preset = "p5";   // NVENC p1 (rápido) .. p7 (calidad)
};

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    // bgra: imagen en memoria normal (B,G,R,A por píxel). out vacío = aún no hay paquete.
    virtual bool encode(const uint8_t* bgra, int stride, int64_t ptsUs, std::vector<uint8_t>& out, bool& key) = 0;
    virtual std::string name() const = 0;
    // El próximo frame será un keyframe (al (re)conectar un destino, para no esperar al siguiente)
    void requestKeyframe() { forceKey_ = true; }
    std::vector<uint8_t> header;   // SPS/PPS (Annex B)

protected:
    std::atomic<bool> forceKey_{false};
};

// kind: "auto" (NVENC y si no x264), "nvenc" o "x264"
std::unique_ptr<VideoEncoder> createVideoEncoder(const std::string& kind, const VideoEncoderParams& p, std::string& err);
