#include "camera.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>

#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

std::string fourccName(uint32_t f) {
    if (f == V4L2_PIX_FMT_MJPEG) return "MJPEG";
    if (f == V4L2_PIX_FMT_YUYV) return "YUYV";
    char b[5] = {(char)(f & 255), (char)((f >> 8) & 255), (char)((f >> 16) & 255), (char)((f >> 24) & 255), 0};
    return b;
}

// ---------------------------------------------------------------------------
// Enumeración de cámaras y modos (resolución + fps) que ofrecen
// ---------------------------------------------------------------------------
std::vector<CamDevice> listCameras() {
    std::vector<CamDevice> r;
    for (int i = 0; i < 64; i++) {
        std::string path = "/dev/video" + std::to_string(i);
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { ::close(fd); continue; }
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) { ::close(fd); continue; }
        CamDevice d;
        d.path = path;
        d.name = (const char*)cap.card;
        std::set<std::tuple<uint32_t, int, int, int>> seen;
        for (uint32_t fi = 0;; fi++) {
            v4l2_fmtdesc fd2{};
            fd2.index = fi;
            fd2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (xioctl(fd, VIDIOC_ENUM_FMT, &fd2) < 0) break;
            if (fd2.pixelformat != V4L2_PIX_FMT_MJPEG && fd2.pixelformat != V4L2_PIX_FMT_YUYV) continue;
            for (uint32_t si = 0;; si++) {
                v4l2_frmsizeenum fs{};
                fs.index = si;
                fs.pixel_format = fd2.pixelformat;
                if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0 || fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
                for (uint32_t ii = 0;; ii++) {
                    v4l2_frmivalenum iv{};
                    iv.index = ii;
                    iv.pixel_format = fd2.pixelformat;
                    iv.width = fs.discrete.width;
                    iv.height = fs.discrete.height;
                    if (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) < 0 || iv.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
                    if (!iv.discrete.numerator) continue;
                    int fps = (int)std::lround((double)iv.discrete.denominator / iv.discrete.numerator);
                    auto key = std::make_tuple(fd2.pixelformat, (int)fs.discrete.width, (int)fs.discrete.height, fps);
                    if (fps < 5 || !seen.insert(key).second) continue;
                    d.modes.push_back({fd2.pixelformat, (int)fs.discrete.width, (int)fs.discrete.height, fps});
                }
            }
        }
        ::close(fd);
        if (!d.modes.empty()) r.push_back(std::move(d));
    }
    return r;
}

// ---------------------------------------------------------------------------
// Captura
// ---------------------------------------------------------------------------
struct Camera::Impl {
    AVCodecContext* dec = nullptr;
    AVFrame* fr = nullptr;
    AVPacket* pkt = nullptr;
    std::vector<uint8_t> jpeg;
    SwsContext* sws = nullptr;
    SwsContext* swsConfigured = nullptr;
    int lastRange = -1;
    ~Impl() {
        sws_freeContext(sws);
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
    }
};

void Camera::start(const CameraParams& p, FrameCb cb) {
    stop();
    p_ = p;
    cb_ = std::move(cb);
    quit_ = false;
    fps_ = 0;
    impl_ = new Impl;
    setStatus("abriendo " + p.device + "…");
    thr_ = std::thread(&Camera::loop, this);
}

void Camera::stop() {
    quit_ = true;
    if (thr_.joinable()) thr_.join();
    delete impl_;
    impl_ = nullptr;
    fps_ = 0;
}

std::string Camera::status() {
    std::lock_guard<std::mutex> l(sm_);
    return status_;
}

void Camera::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(sm_);
    if (s != status_) LOG("cámara: %s", s.c_str());
    status_ = s;
}

bool Camera::openDev() {
    fd_ = ::open(p_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) { setStatus("no se puede abrir " + p_.device + ": " + strerror(errno) + " (se reintentará)"); return false; }
    v4l2_format f{};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = (uint32_t)p_.w;
    f.fmt.pix.height = (uint32_t)p_.h;
    f.fmt.pix.pixelformat = p_.fourcc ? p_.fourcc : V4L2_PIX_FMT_MJPEG;
    f.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(fd_, VIDIOC_S_FMT, &f) < 0) { setStatus(std::string("VIDIOC_S_FMT: ") + strerror(errno)); return false; }
    if (f.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG && f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        setStatus("la cámara no da YUYV ni MJPEG");
        return false;
    }
    mjpeg_ = f.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG;
    capW_ = (int)f.fmt.pix.width;
    capH_ = (int)f.fmt.pix.height;
    stride_ = f.fmt.pix.bytesperline ? (int)f.fmt.pix.bytesperline : capW_ * 2;

    v4l2_streamparm sp{};
    sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sp.parm.capture.timeperframe.numerator = 1;
    sp.parm.capture.timeperframe.denominator = (uint32_t)p_.fps;
    xioctl(fd_, VIDIOC_S_PARM, &sp);
    double realFps = sp.parm.capture.timeperframe.numerator
                         ? (double)sp.parm.capture.timeperframe.denominator / sp.parm.capture.timeperframe.numerator
                         : p_.fps;

    // Con poca luz muchas webcams alargan la exposición y bajan a 30 fps o
    // menos. Desactivar la "prioridad de exposición" mantiene los fps pedidos.
    v4l2_control ctl{};
    ctl.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
    ctl.value = 0;
    xioctl(fd_, VIDIOC_S_CTRL, &ctl);

    v4l2_requestbuffers rb{};
    rb.count = 4;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) { setStatus("VIDIOC_REQBUFS falló"); return false; }
    for (uint32_t i = 0; i < rb.count; i++) {
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) { setStatus("VIDIOC_QUERYBUF falló"); return false; }
        void* m = mmap(nullptr, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, b.m.offset);
        if (m == MAP_FAILED) { setStatus("mmap falló"); return false; }
        bufs_.push_back({m, b.length});
        if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) { setStatus("VIDIOC_QBUF falló"); return false; }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        setStatus(std::string("VIDIOC_STREAMON: ") + strerror(errno) + " (¿la usa otro programa?)");
        return false;
    }
    char b[200];
    snprintf(b, sizeof b, "%s %dx%d %s @ %.0f fps -> %dx%d", p_.device.c_str(), capW_, capH_, mjpeg_ ? "MJPEG" : "YUYV",
             realFps, p_.outW, p_.outH);
    setStatus(b);
    out_.assign((size_t)p_.outW * p_.outH * 4, 0);
    fpsT0_ = nowUs();
    fpsN_ = 0;
    return true;
}

void Camera::closeDev() {
    if (fd_ >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }
    for (auto& b : bufs_) munmap(b.first, b.second);
    bufs_.clear();
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    fps_ = 0;
}

void Camera::process(const uint8_t* data, size_t size, int64_t tsUs) {
    Impl& I = *impl_;
    AVPixelFormat fmt;
    const uint8_t* planes[4] = {nullptr, nullptr, nullptr, nullptr};
    int strides[4] = {0, 0, 0, 0};
    int w, h;
    bool fullRange = false;
    if (mjpeg_) {
        if (!I.dec) {
            const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
            if (!c) return;
            I.dec = avcodec_alloc_context3(c);
            I.dec->thread_count = 2;
            if (avcodec_open2(I.dec, c, nullptr) < 0) { avcodec_free_context(&I.dec); return; }
            I.fr = av_frame_alloc();
            I.pkt = av_packet_alloc();
        }
        I.jpeg.assign(data, data + size);
        I.jpeg.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        I.pkt->data = I.jpeg.data();
        I.pkt->size = (int)size;
        if (avcodec_send_packet(I.dec, I.pkt) < 0) return;
        if (avcodec_receive_frame(I.dec, I.fr) < 0) return;
        fmt = (AVPixelFormat)I.fr->format;
        fullRange = I.fr->color_range == AVCOL_RANGE_JPEG;
        // los formatos "yuvj" están obsoletos: se usa el normal indicando rango completo
        if (fmt == AV_PIX_FMT_YUVJ420P) fmt = AV_PIX_FMT_YUV420P, fullRange = true;
        else if (fmt == AV_PIX_FMT_YUVJ422P) fmt = AV_PIX_FMT_YUV422P, fullRange = true;
        else if (fmt == AV_PIX_FMT_YUVJ444P) fmt = AV_PIX_FMT_YUV444P, fullRange = true;
        w = I.fr->width;
        h = I.fr->height;
        for (int i = 0; i < 4; i++) { planes[i] = I.fr->data[i]; strides[i] = I.fr->linesize[i]; }
    } else {
        if (size < (size_t)stride_ * capH_) return;
        fmt = AV_PIX_FMT_YUYV422;
        w = capW_;
        h = capH_;
        planes[0] = data;
        strides[0] = stride_;
    }
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    if (!d) return;

    // Recorte centrado para conservar la proporción de la salida (p. ej. 4:3 -> 16:9)
    int cw = w, ch = h, cx = 0, cy = 0;
    if ((int64_t)w * p_.outH > (int64_t)p_.outW * h) {
        cw = (int)((int64_t)h * p_.outW / p_.outH) & ~1;
        cx = ((w - cw) / 2) & ~1;
    } else if ((int64_t)w * p_.outH < (int64_t)p_.outW * h) {
        ch = (int)((int64_t)w * p_.outH / p_.outW) & ~1;
        cy = ((h - ch) / 2) & ~1;
    }
    if (cx || cy) {
        if (fmt == AV_PIX_FMT_YUYV422) {
            planes[0] += (size_t)cy * strides[0] + (size_t)cx * 2;
        } else {
            for (int i = 0; i < 3 && planes[i]; i++) {
                int sx = i ? d->log2_chroma_w : 0, sy = i ? d->log2_chroma_h : 0;
                planes[i] += (size_t)(cy >> sy) * strides[i] + (size_t)(cx >> sx);
            }
        }
    }

    I.sws = sws_getCachedContext(I.sws, cw, ch, fmt, p_.outW, p_.outH, AV_PIX_FMT_BGRA, SWS_BICUBIC, nullptr, nullptr,
                                 nullptr);
    if (!I.sws) return;
    if (I.sws != I.swsConfigured || I.lastRange != (int)fullRange) {
        // webcams: BT.601; MJPEG en rango completo, YUYV normalmente limitado
        sws_setColorspaceDetails(I.sws, sws_getCoefficients(SWS_CS_ITU601), fullRange ? 1 : 0,
                                 sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);
        I.swsConfigured = I.sws;
        I.lastRange = fullRange;
    }
    uint8_t* dst[1] = {out_.data()};
    int dstStride[1] = {p_.outW * 4};
    sws_scale(I.sws, planes, strides, 0, ch, dst, dstStride);
    if (cb_) cb_(out_.data(), p_.outW, p_.outH, p_.outW * 4, tsUs);

    fpsN_++;
    uint64_t now = nowUs();
    if (now - fpsT0_ >= 1000000) {
        fps_ = fpsN_ * 1e6 / (double)(now - fpsT0_);
        fpsN_ = 0;
        fpsT0_ = now;
    }
}

void Camera::loop() {
    while (!quit_) {
        if (!openDev()) {
            closeDev();
            for (int i = 0; i < 30 && !quit_; i++) usleep(100000);   // reintento cada 3 s
            continue;
        }
        int timeouts = 0;
        while (!quit_) {
            fd_set fs;
            FD_ZERO(&fs);
            FD_SET(fd_, &fs);
            timeval tv{0, 200000};
            int r = select(fd_ + 1, &fs, nullptr, nullptr, &tv);
            if (r < 0 && errno != EINTR) break;
            if (r <= 0) {
                if (++timeouts == 15) setStatus("la cámara no envía imágenes (¿desconectada?)");
                if (timeouts > 25) break;   // 5 s sin imagen: reabrir
                continue;
            }
            timeouts = 0;
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) {
                if (errno == EAGAIN) continue;
                setStatus(std::string("se ha desconectado (") + strerror(errno) + "), se reintentará");
                break;
            }
            int64_t ts;
            if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
                (b.timestamp.tv_sec || b.timestamp.tv_usec))
                ts = (int64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
            else
                ts = (int64_t)nowUs();
            if (b.index < bufs_.size() && !(b.flags & V4L2_BUF_FLAG_ERROR) && b.bytesused > 0)
                process((const uint8_t*)bufs_[b.index].first, b.bytesused, ts);
            xioctl(fd_, VIDIOC_QBUF, &b);
        }
        closeDev();
    }
    closeDev();
}
