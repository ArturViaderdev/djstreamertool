#include "audio.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"

int AudioSourceInfo::bits() const {
    if (format.rfind("s16", 0) == 0 || format.rfind("u16", 0) == 0) return 16;
    if (format.rfind("s24", 0) == 0) return 24;
    if (format.rfind("u8", 0) == 0) return 8;
    return 32;
}

static std::string run(const std::string& cmd) {
    std::string r;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, p)) > 0) r.append(b, n);
        pclose(p);
    }
    return r;
}

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<AudioSourceInfo> listAudioSources() {
    // LC_ALL=C: pactl traduce las etiquetas ("Nombre:", "Descripción:"...)
    std::string out = run("LC_ALL=C pactl list sources 2>/dev/null");
    std::vector<AudioSourceInfo> r;
    AudioSourceInfo cur;
    bool in = false, monitor = false;
    auto flush = [&] {
        if (in && !cur.name.empty() && !monitor) r.push_back(cur);
        cur = AudioSourceInfo();
        monitor = false;
    };
    size_t pos = 0;
    while (pos < out.size()) {
        size_t e = out.find('\n', pos);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(pos, e - pos);
        pos = e + 1;
        if (line.rfind("Source #", 0) == 0) { flush(); in = true; continue; }
        std::string t = trim(line);
        auto val = [&](const char* k) -> const char* {
            size_t n = strlen(k);
            return t.compare(0, n, k) == 0 ? t.c_str() + n : nullptr;
        };
        if (const char* v = val("Name:")) {
            cur.name = trim(v);
            if (cur.name.size() > 8 && cur.name.compare(cur.name.size() - 8, 8, ".monitor") == 0) monitor = true;
        } else if (const char* v = val("Description:")) {
            cur.description = trim(v);
        } else if (const char* v = val("Monitor of Sink:")) {
            if (trim(v) != "n/a") monitor = true;
        } else if (const char* v = val("Sample Specification:")) {
            char fmt[32] = {0};
            int ch = 0, rate = 0;
            if (sscanf(v, " %31s %dch %dHz", fmt, &ch, &rate) == 3) {
                cur.format = fmt;
                cur.channels = ch;
                cur.rate = rate;
            }
        }
    }
    flush();
    return r;
}

std::string defaultAudioSource() {
    return trim(run("pactl get-default-source 2>/dev/null"));
}

// ---------------------------------------------------------------------------
// libpulse-simple con dlopen (de gamecapture), con frecuencia configurable
// ---------------------------------------------------------------------------
namespace {
class PulseCapture {
public:
    ~PulseCapture() { close(); }

    bool open(const std::string& dev, int rate, std::string& err) {
        if (!lib_) {
            lib_ = dlopen("libpulse-simple.so.0", RTLD_NOW);
            if (!lib_) { err = "no está libpulse-simple.so.0 (paquete libpulse0)"; return false; }
            new_ = (void* (*)(const char*, const char*, int, const char*, const char*, const SampleSpec*, const void*,
                              const BufferAttr*, int*))dlsym(lib_, "pa_simple_new");
            read_ = (int (*)(void*, void*, size_t, int*))dlsym(lib_, "pa_simple_read");
            free_ = (void (*)(void*))dlsym(lib_, "pa_simple_free");
            latency_ = (uint64_t (*)(void*, int*))dlsym(lib_, "pa_simple_get_latency");
            void* core = dlopen("libpulse.so.0", RTLD_NOW);
            if (core) strerror_ = (const char* (*)(int))dlsym(core, "pa_strerror");
            if (!new_ || !read_ || !free_) { err = "libpulse-simple incompleta"; return false; }
        }
        SampleSpec ss{5 /* PA_SAMPLE_FLOAT32LE */, (uint32_t)rate, 2};
        BufferAttr ba{(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)(rate / 100 * 8) /* 10 ms */};
        int e = 0;
        s_ = new_(nullptr, "DJ Live Streamer", 2 /* PA_STREAM_RECORD */, dev.empty() ? nullptr : dev.c_str(),
                  "entrada de línea", &ss, nullptr, &ba, &e);
        if (!s_) {
            err = "no se pudo abrir '" + dev + "': " + (strerror_ ? strerror_(e) : std::to_string(e));
            return false;
        }
        return true;
    }
    bool read(float* interleaved, int frames) {
        int e = 0;
        return s_ && read_(s_, interleaved, (size_t)frames * 8, &e) >= 0;
    }
    uint64_t latencyUs() {
        if (!s_ || !latency_) return 0;
        int e = 0;
        uint64_t l = latency_(s_, &e);
        return l == (uint64_t)-1 || l > 500000 ? 0 : l;
    }
    void close() {
        if (s_) free_(s_);
        s_ = nullptr;
    }

private:
    struct SampleSpec { int format; uint32_t rate; uint8_t channels; };
    struct BufferAttr { uint32_t maxlength, tlength, prebuf, minreq, fragsize; };
    void* lib_ = nullptr;
    void* s_ = nullptr;
    void* (*new_)(const char*, const char*, int, const char*, const char*, const SampleSpec*, const void*,
                  const BufferAttr*, int*) = nullptr;
    int (*read_)(void*, void*, size_t, int*) = nullptr;
    void (*free_)(void*) = nullptr;
    uint64_t (*latency_)(void*, int*) = nullptr;
    const char* (*strerror_)(int) = nullptr;
};
}   // namespace

void AudioCapture::start(const std::string& source, int rate, BlockCb cb) {
    stop();
    source_ = source;
    rate_ = rate > 0 ? rate : 48000;
    cb_ = std::move(cb);
    quit_ = false;
    thr_ = std::thread(&AudioCapture::loop, this);
}

void AudioCapture::stop() {
    quit_ = true;
    if (thr_.joinable()) thr_.join();
}

std::string AudioCapture::status() {
    std::lock_guard<std::mutex> l(sm_);
    return status_;
}

void AudioCapture::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(sm_);
    if (s != status_) LOG("audio: %s", s.c_str());
    status_ = s;
}

void AudioCapture::takeLevels(float peak[2], bool clip[2]) {
    std::lock_guard<std::mutex> l(lm_);
    for (int c = 0; c < 2; c++) {
        peak[c] = peak_[c];
        clip[c] = clip_[c];
        peak_[c] = 0;
        clip_[c] = false;
    }
}

void AudioCapture::loop() {
    // ~-0,01 dBFS: el valor máximo que da un conversor de 16 o 24 bits ya cuenta
    // como saturación (y lo que pase de 1.0 es volumen digital por encima de 100 %)
    const float kClip = 0.999f;
    const int frames = rate_ / 100;   // bloques de 10 ms
    std::vector<float> buf((size_t)frames * 2);
    while (!quit_) {
        PulseCapture cap;
        std::string err;
        if (!cap.open(source_, rate_, err)) {
            setStatus(err + " (se reintentará)");
            for (int i = 0; i < 20 && !quit_; i++) usleep(100000);
            continue;
        }
        setStatus("capturando " + source_ + " a " + std::to_string(rate_) + " Hz");
        while (!quit_) {
            if (!cap.read(buf.data(), frames)) {
                setStatus("error de lectura, se reabre la entrada");
                break;
            }
            // instante de la 1ª muestra = llegada - retraso del sistema de sonido - duración del bloque
            int64_t cap0 = (int64_t)nowUs() - (int64_t)cap.latencyUs() - (int64_t)frames * 1000000 / rate_;
            float pk[2] = {0, 0};
            bool cl[2] = {false, false};
            for (int i = 0; i < frames; i++)
                for (int c = 0; c < 2; c++) {
                    float a = std::fabs(buf[2 * i + c]);
                    if (a > pk[c]) pk[c] = a;
                    if (a >= kClip) cl[c] = true;
                }
            {
                std::lock_guard<std::mutex> l(lm_);
                for (int c = 0; c < 2; c++) {
                    peak_[c] = std::max(peak_[c], pk[c]);
                    clip_[c] = clip_[c] || cl[c];
                }
            }
            if (cb_) cb_(buf.data(), frames, cap0);
        }
        cap.close();
    }
}
