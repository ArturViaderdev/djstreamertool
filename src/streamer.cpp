#include "streamer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>

#include "encoder.h"
#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

// Convención interna de los paquetes en cola: pts/dts en microsegundos,
// stream_index 0 = vídeo, 1 = audio.
static constexpr AVRational kUs = {1, 1000000};

struct Streamer::Sink {
    StreamDest dest;
    int venc = 0;
    std::thread thr;
    std::mutex m;
    std::condition_variable cv;
    std::deque<AVPacket*> q;
    bool stop = false, dropUntilKey = false;
    std::atomic<bool> live{false};
    std::atomic<int> reconnects{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> abortAt{0};   // para cortar conexiones colgadas al parar
    std::mutex sm;
    std::string state = "conectando…";
    uint64_t lastBytes = 0, lastT = 0;
    double kbps = 0;

    void setState(const std::string& s) {
        std::lock_guard<std::mutex> l(sm);
        state = s;
    }
};

static int interruptCb(void* o) {
    auto* s = (Streamer::Sink*)o;
    uint64_t a = s->abortAt.load();
    return a && nowUs() > a;
}

static void closeOutput(AVFormatContext*& oc, bool trailer) {
    if (!oc) return;
    if (trailer) av_write_trailer(oc);
    if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
    avformat_free_context(oc);
    oc = nullptr;
}

static AVFormatContext* openOutput(Streamer::Sink* s, const std::vector<uint8_t>& header, const StreamConfig& cfg,
                                   const AVCodecParameters* apar, std::string& err) {
    const std::string& url = s->dest.url;
    const bool rtmp = url.rfind("rtmp", 0) == 0;
    AVFormatContext* oc = nullptr;
    int r = avformat_alloc_output_context2(&oc, nullptr, rtmp ? "flv" : nullptr, url.c_str());
    if (r < 0 || !oc) r = avformat_alloc_output_context2(&oc, nullptr, "flv", url.c_str());
    if (r < 0 || !oc) { err = "no se pudo crear la salida: " + averr(r); return nullptr; }
    oc->interrupt_callback = {interruptCb, s};
    oc->max_interleave_delta = 1000000;   // nunca retener vídeo más de 1 s esperando al audio

    AVStream* vst = avformat_new_stream(oc, nullptr);
    AVCodecParameters* vp = vst->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = AV_CODEC_ID_H264;
    vp->width = cfg.width;
    vp->height = cfg.height;
    vp->bit_rate = (int64_t)s->dest.videoKbps * 1000;
    vp->extradata = (uint8_t*)av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(vp->extradata, header.data(), header.size());
    vp->extradata_size = (int)header.size();
    vst->avg_frame_rate = {cfg.fps, 1};
    vst->time_base = {1, 1000};
    AVStream* ast = avformat_new_stream(oc, nullptr);
    avcodec_parameters_copy(ast->codecpar, apar);
    ast->time_base = {1, 1000};

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary* o = nullptr;
        if (rtmp) av_dict_set(&o, "rw_timeout", "10000000", 0);
        r = avio_open2(&oc->pb, url.c_str(), AVIO_FLAG_WRITE, &oc->interrupt_callback, &o);
        av_dict_free(&o);
        if (r < 0) {
            err = "no se pudo conectar: " + averr(r);
            avformat_free_context(oc);
            return nullptr;
        }
    }
    AVDictionary* mo = nullptr;
    if (rtmp) av_dict_set(&mo, "flvflags", "no_duration_filesize", 0);
    r = avformat_write_header(oc, &mo);
    av_dict_free(&mo);
    if (r < 0) {
        err = "error al iniciar: " + averr(r);
        closeOutput(oc, false);
        return nullptr;
    }
    return oc;
}

Streamer::Streamer() = default;
Streamer::~Streamer() { stop(); }

bool Streamer::start(const StreamConfig& c, std::string& err) {
    stop();
    cfg_ = c;
    if (cfg_.dests.empty()) { err = "no hay ningún destino"; return false; }
    audioShiftUs_ = cfg_.avOffsetMs > 0 ? (int64_t)cfg_.avOffsetMs * 1000 : 0;
    videoShiftUs_ = cfg_.avOffsetMs < 0 ? -(int64_t)cfg_.avOffsetMs * 1000 : 0;

    // Un codificador por cada tasa de bits distinta (Twitch y YouTube suelen querer distinta)
    std::vector<int> sinkEnc;
    for (auto& d : cfg_.dests) {
        int idx = -1;
        for (size_t i = 0; i < rates_.size(); i++)
            if (rates_[i] == d.videoKbps) idx = (int)i;
        if (idx < 0) {
            VideoEncoderParams p;
            p.w = cfg_.width;
            p.h = cfg_.height;
            p.fps = cfg_.fps;
            p.kbps = d.videoKbps;
            p.keyintSec = cfg_.keyintSec;
            p.preset = cfg_.preset;
            std::string e;
            auto enc = createVideoEncoder(cfg_.encoder, p, e);
            if (!enc) {
                err = "codificador de vídeo: " + e;
                encs_.clear();
                rates_.clear();
                return false;
            }
            encs_.push_back(std::move(enc));
            rates_.push_back(d.videoKbps);
            idx = (int)encs_.size() - 1;
        }
        sinkEnc.push_back(idx);
    }
    encName_ = encs_[0]->name();

    // AAC a la frecuencia de la entrada si la admiten las plataformas (44,1 / 48 kHz)
    outRate_ = (cfg_.inputRate == 44100 || cfg_.inputRate == 48000) ? cfg_.inputRate : 48000;
    if (outRate_ != cfg_.inputRate) {
        AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr_, &st, AV_SAMPLE_FMT_FLT, outRate_, &st, AV_SAMPLE_FMT_FLT, cfg_.inputRate, 0,
                                nullptr) < 0 ||
            swr_init(swr_) < 0) {
            err = "no se pudo crear el remuestreador de audio";
            swr_free(&swr_);
            encs_.clear();
            rates_.clear();
            return false;
        }
        LOG("audio del directo: %d Hz -> %d Hz", cfg_.inputRate, outRate_);
    }
    const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    aenc_ = avcodec_alloc_context3(ac);
    aenc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aenc_->sample_rate = outRate_;
    AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&aenc_->ch_layout, &st);
    aenc_->bit_rate = (int64_t)cfg_.audioKbps * 1000;
    aenc_->time_base = {1, outRate_};
    aenc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(aenc_->priv_data, "aac_coder", "twoloop", 0);   // mejor calidad (música)
    int r = avcodec_open2(aenc_, ac, nullptr);
    if (r < 0) {
        err = "AAC: " + averr(r);
        avcodec_free_context(&aenc_);
        swr_free(&swr_);
        encs_.clear();
        rates_.clear();
        return false;
    }
    apar_ = avcodec_parameters_alloc();
    avcodec_parameters_from_context(apar_, aenc_);
    afr_ = av_frame_alloc();
    afr_->nb_samples = aenc_->frame_size;
    afr_->format = AV_SAMPLE_FMT_FLTP;
    afr_->sample_rate = outRate_;
    av_channel_layout_copy(&afr_->ch_layout, &st);
    av_frame_get_buffer(afr_, 0);

    for (size_t i = 0; i < cfg_.dests.size(); i++) {
        auto s = std::make_unique<Sink>();
        s->dest = cfg_.dests[i];
        s->venc = sinkEnc[i];
        sinks_.push_back(std::move(s));
    }

    t0_ = nowUs();
    frames_ = 0;
    dropped_ = 0;
    fpsLastT_ = t0_;
    fpsLastN_ = 0;
    fps_ = 0;
    aStarted_ = false;
    inCount_ = outCount_ = 0;
    drift_ = 0;
    nDrift_ = 0;
    driftUs_ = 0;
    fifoL_.clear();
    fifoR_.clear();
    haveFrame_ = false;
    vactive_ = true;
    aactive_ = true;
    active_ = true;
    for (auto& s : sinks_) s->thr = std::thread(&Streamer::writerLoop, this, s.get());
    encThr_ = std::thread(&Streamer::encodeLoop, this);

    std::string rs, names;
    for (int x : rates_) rs += (rs.empty() ? "" : "/") + std::to_string(x);
    for (auto& d : cfg_.dests) names += (names.empty() ? "" : " + ") + d.name;
    LOG("directo: %dx%d @ %d fps, %s %s kbps, AAC %d kbps %d Hz -> %s", cfg_.width, cfg_.height, cfg_.fps,
        encName_.c_str(), rs.c_str(), cfg_.audioKbps, outRate_, names.c_str());
    return true;
}

void Streamer::stop() {
    if (!active_ && !encThr_.joinable() && sinks_.empty()) return;
    active_ = false;
    {
        std::lock_guard<std::mutex> l(am_);
        aactive_ = false;
        if (aenc_) {
            avcodec_send_frame(aenc_, nullptr);
            drainAudio();
        }
    }
    {
        std::lock_guard<std::mutex> l(vm_);
        vactive_ = false;
    }
    vcv_.notify_all();
    if (encThr_.joinable()) encThr_.join();

    // Se da a cada destino hasta 4 s para vaciar su cola y cerrar limpio
    const uint64_t deadline = nowUs() + 4000000;
    for (auto& s : sinks_) {
        {
            std::lock_guard<std::mutex> l(s->m);
            s->stop = true;
        }
        s->abortAt = deadline;
        s->cv.notify_all();
    }
    for (auto& s : sinks_)
        if (s->thr.joinable()) s->thr.join();
    {
        std::lock_guard<std::mutex> l(stMtx_);
        sinks_.clear();
    }
    encs_.clear();
    rates_.clear();
    avcodec_free_context(&aenc_);
    avcodec_parameters_free(&apar_);
    av_frame_free(&afr_);
    swr_free(&swr_);
    fifoL_.clear();
    fifoR_.clear();
    LOG("directo terminado (%llu frames, %.0f s)", (unsigned long long)frames_.load(), (nowUs() - t0_) / 1e6);
}

// ---------------------------------------------------------------------------
// Vídeo
// ---------------------------------------------------------------------------
void Streamer::pushVideo(const uint8_t* bgra, int stride, int w, int h, int64_t tsUs) {
    if (!active_ || w != cfg_.width || h != cfg_.height) return;
    std::lock_guard<std::mutex> l(vm_);
    if (!vactive_) return;
    if (haveFrame_) dropped_++;   // el codificador no ha llegado a tiempo: se queda la más nueva
    pending_.resize((size_t)w * h * 4);
    if (stride == w * 4) memcpy(pending_.data(), bgra, pending_.size());
    else
        for (int y = 0; y < h; y++) memcpy(pending_.data() + (size_t)y * w * 4, bgra + (size_t)y * stride, (size_t)w * 4);
    pendingTs_ = tsUs;
    haveFrame_ = true;
    vcv_.notify_one();
}

void Streamer::encodeLoop() {
    const int w = cfg_.width, h = cfg_.height;
    std::vector<uint8_t> frame((size_t)w * h * 4, 0);   // negro mientras no llegue la cámara
    for (size_t i = 3; i < frame.size(); i += 4) frame[i] = 255;
    std::vector<uint8_t> bits;
    int64_t lastPts = -1;
    bool warned = false;
    while (true) {
        int64_t ts;
        {
            std::unique_lock<std::mutex> l(vm_);
            vcv_.wait_for(l, std::chrono::milliseconds(250), [&] { return !vactive_ || haveFrame_; });
            if (!vactive_) break;
            if (haveFrame_) {
                frame.swap(pending_);
                ts = pendingTs_;
                haveFrame_ = false;
            } else {
                // Sin imagen de la cámara (desconectada...): se repite la última
                // para que el directo no se corte; el audio sigue normal.
                ts = (int64_t)nowUs();
            }
        }
        // Reloj del sistema -> reloj del audio (corrige la deriva entre la webcam y la tarjeta)
        int64_t pts = ts - (int64_t)t0_ + driftUs_.load() + videoShiftUs_;
        if (pts < 0) continue;
        if (pts < lastPts + 1000) pts = lastPts + 1000;
        lastPts = pts;
        for (size_t i = 0; i < encs_.size(); i++) {
            bool key = false;
            if (!encs_[i]->encode(frame.data(), w * 4, pts, bits, key)) {
                if (!warned) LOG("error del codificador de vídeo (%s)", encs_[i]->name().c_str());
                warned = true;
                continue;
            }
            if (bits.empty()) continue;
            AVPacket* p = av_packet_alloc();
            if (av_new_packet(p, (int)bits.size()) < 0) { av_packet_free(&p); continue; }
            memcpy(p->data, bits.data(), bits.size());
            p->pts = p->dts = pts;
            p->stream_index = 0;
            if (key) p->flags |= AV_PKT_FLAG_KEY;
            distribute(p, true, (int)i);
        }
        frames_++;
    }
}

double Streamer::encodeFps() {
    uint64_t now = nowUs(), n = frames_;
    if (now - fpsLastT_ >= 900000) {
        fps_ = (n - fpsLastN_) * 1e6 / (double)(now - fpsLastT_);
        fpsLastT_ = now;
        fpsLastN_ = n;
    }
    return fps_;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
void Streamer::pushAudio(const float* in, int frames, int64_t captureUs) {
    std::lock_guard<std::mutex> l(am_);
    if (!aactive_) return;
    const int64_t rel = captureUs - (int64_t)t0_;
    if (!aStarted_) {
        if (rel < 0) return;   // bloque capturado antes de pulsar "empezar"
        aStarted_ = true;
        a0Us_ = rel;
    }
    // Deriva: dónde dice el reloj de la tarjeta que estamos vs el reloj del sistema
    const double expected = a0Us_ + inCount_ * 1e6 / cfg_.inputRate;
    const double d = expected - (double)rel;
    if (nDrift_ < 300) drift_ = (drift_ * nDrift_ + d) / (nDrift_ + 1);   // primeros 3 s: media
    else drift_ += 0.003 * (d - drift_);                                   // luego media móvil (~3 s)
    nDrift_++;
    driftUs_ = (int64_t)std::llround(drift_);
    inCount_ += frames;

    if (swr_) {
        int maxOut = swr_get_out_samples(swr_, frames);
        std::vector<float> tmp((size_t)std::max(maxOut, 0) * 2 + 2);
        uint8_t* o[1] = {(uint8_t*)tmp.data()};
        const uint8_t* i[1] = {(const uint8_t*)in};
        int n = swr_convert(swr_, o, maxOut, i, frames);
        for (int k = 0; k < n; k++) {
            fifoL_.push_back(tmp[2 * k]);
            fifoR_.push_back(tmp[2 * k + 1]);
        }
    } else {
        for (int k = 0; k < frames; k++) {
            fifoL_.push_back(in[2 * k]);
            fifoR_.push_back(in[2 * k + 1]);
        }
    }

    const int fs = aenc_->frame_size;
    const int64_t base = std::llround(a0Us_ * (double)outRate_ / 1e6) + audioShiftUs_ * outRate_ / 1000000;
    size_t off = 0;
    while (fifoL_.size() - off >= (size_t)fs) {
        if (av_frame_make_writable(afr_) < 0) break;
        float* L = (float*)afr_->data[0];
        float* R = (float*)afr_->data[1];
        for (int k = 0; k < fs; k++) {
            // el AAC no admite valores fuera de [-1, 1]: lo que pase ya está saturado
            L[k] = std::clamp(fifoL_[off + k], -1.f, 1.f);
            R[k] = std::clamp(fifoR_[off + k], -1.f, 1.f);
        }
        off += fs;
        afr_->pts = base + outCount_;
        outCount_ += fs;
        if (avcodec_send_frame(aenc_, afr_) >= 0) drainAudio();
    }
    fifoL_.erase(fifoL_.begin(), fifoL_.begin() + (ptrdiff_t)off);
    fifoR_.erase(fifoR_.begin(), fifoR_.begin() + (ptrdiff_t)off);
}

void Streamer::drainAudio() {
    AVPacket* p = av_packet_alloc();
    while (avcodec_receive_packet(aenc_, p) >= 0) {
        AVPacket* c = av_packet_alloc();
        av_packet_move_ref(c, p);
        c->pts = av_rescale_q(c->pts, aenc_->time_base, kUs);
        c->dts = av_rescale_q(c->dts, aenc_->time_base, kUs);
        c->duration = av_rescale_q(c->duration, aenc_->time_base, kUs);
        c->stream_index = 1;
        distribute(c, false, 0);
    }
    av_packet_free(&p);
}

// ---------------------------------------------------------------------------
// Destinos
// ---------------------------------------------------------------------------
void Streamer::distribute(AVPacket* p, bool video, int venc) {
    for (auto& sp : sinks_) {
        Sink& s = *sp;
        if (video && s.venc != venc) continue;
        AVPacket* c = av_packet_clone(p);
        if (!c) continue;
        std::lock_guard<std::mutex> l(s.m);
        if (s.q.size() > 1500) s.dropUntilKey = true;   // ~15 s atascado: soltar lastre
        if (s.dropUntilKey) {
            if (video && (c->flags & AV_PKT_FLAG_KEY) && s.q.size() < 300) s.dropUntilKey = false;
            else {
                av_packet_free(&c);
                continue;
            }
        }
        s.q.push_back(c);
        s.cv.notify_one();
    }
    av_packet_free(&p);
}

void Streamer::writerLoop(Sink* s) {
    AVFormatContext* oc = nullptr;
    bool waitingKey = true;
    int64_t base = 0;
    uint64_t nextTry = 0;
    std::string lastErr;
    const std::vector<uint8_t> header = encs_[s->venc]->header;
    while (true) {
        AVPacket* p = nullptr;
        {
            std::unique_lock<std::mutex> l(s->m);
            s->cv.wait_for(l, std::chrono::milliseconds(200), [&] { return s->stop || !s->q.empty(); });
            if (s->stop && (s->q.empty() || !oc)) break;
            if (!s->q.empty()) {
                p = s->q.front();
                s->q.pop_front();
            }
        }
        if (!oc) {
            if (p) av_packet_free(&p);   // sin conexión: se descarta
            if (nowUs() < nextTry) continue;
            s->setState(s->reconnects ? "reconectando…" : "conectando…");
            std::string e;
            oc = openOutput(s, header, cfg_, apar_, e);
            if (!oc) {
                if (s->stop) break;
                if (e != lastErr) LOG("%s: %s (se reintenta cada 5 s)", s->dest.name.c_str(), e.c_str());
                lastErr = e;
                s->setState("sin conexión: " + e);
                nextTry = nowUs() + 5000000;
                continue;
            }
            LOG("%s: conectado (%d kbps)", s->dest.name.c_str(), s->dest.videoKbps);
            lastErr.clear();
            // lo acumulado mientras conectaba ya es viejo: empezar desde aquí
            {
                std::lock_guard<std::mutex> l(s->m);
                for (auto* x : s->q) av_packet_free(&x);
                s->q.clear();
                s->dropUntilKey = false;
            }
            waitingKey = true;
            encs_[s->venc]->requestKeyframe();
            s->setState("esperando keyframe…");
            continue;
        }
        if (!p) continue;
        const bool video = p->stream_index == 0;
        if (waitingKey) {
            // cada conexión empieza en un keyframe y con las marcas desde 0
            if (!(video && (p->flags & AV_PKT_FLAG_KEY))) { av_packet_free(&p); continue; }
            waitingKey = false;
            base = p->pts;
            s->live = true;
            s->setState("en directo");
        }
        if (p->pts < base) { av_packet_free(&p); continue; }
        AVStream* st = oc->streams[video ? 0 : 1];
        p->pts -= base;
        p->dts -= base;
        if (p->dts < 0) p->dts = 0;
        av_packet_rescale_ts(p, kUs, st->time_base);
        const int sz = p->size;
        int r = av_interleaved_write_frame(oc, p);
        av_packet_free(&p);
        if (r < 0) {
            if (s->stop) break;
            LOG("%s: se ha cortado la conexión (%s): reconectando", s->dest.name.c_str(), averr(r).c_str());
            closeOutput(oc, false);
            s->live = false;
            s->reconnects++;
            s->setState("cortado, reconectando…");
            nextTry = nowUs() + 2000000;
            continue;
        }
        s->bytes += (uint64_t)sz;
    }
    if (oc) closeOutput(oc, s->live && !interruptCb(s));
    s->live = false;
    std::lock_guard<std::mutex> l(s->m);
    for (auto* x : s->q) av_packet_free(&x);
    s->q.clear();
}

std::vector<SinkStatus> Streamer::status() {
    std::vector<SinkStatus> r;
    std::lock_guard<std::mutex> lk(stMtx_);
    const uint64_t now = nowUs();
    for (auto& sp : sinks_) {
        Sink& s = *sp;
        SinkStatus st;
        st.name = s.dest.name;
        {
            std::lock_guard<std::mutex> l(s.sm);
            st.state = s.state;
        }
        st.live = s.live;
        st.reconnects = s.reconnects;
        uint64_t b = s.bytes;
        if (s.lastT && now > s.lastT + 500000) {
            s.kbps = (b - s.lastBytes) * 8.0 / 1000.0 / ((now - s.lastT) / 1e6);
            s.lastBytes = b;
            s.lastT = now;
        } else if (!s.lastT) {
            s.lastT = now;
            s.lastBytes = b;
        }
        st.kbps = s.kbps;
        r.push_back(st);
    }
    return r;
}
