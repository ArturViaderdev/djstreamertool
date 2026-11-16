#include "audiorecorder.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

struct AudioRecorder::Impl {
    AVFormatContext* oc = nullptr;
    AVCodecContext* enc = nullptr;
    AVStream* st = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* fr = nullptr;
    AVPacket* pkt = nullptr;
    std::vector<uint8_t> fifo;   // muestras ya convertidas (intercaladas)
    std::vector<uint8_t> tmp;
    int frameBytes = 0;          // bytes por muestra estéreo
    bool planar = false;         // el codificador quiere un plano por canal (MP3)
    int64_t pts = 0;
    bool failed = false;

    ~Impl() {
        swr_free(&swr);
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&enc);
        if (oc) {
            if (!(oc->oformat->flags & AVFMT_NOFILE)) avio_closep(&oc->pb);
            avformat_free_context(oc);
        }
    }
};

static SwrContext* makeSwr(int inRate, int outRate, AVSampleFormat outFmt, bool dither, bool soxr) {
    AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* s = nullptr;
    if (swr_alloc_set_opts2(&s, &st, outFmt, outRate, &st, AV_SAMPLE_FMT_FLT, inRate, 0, nullptr) < 0) return nullptr;
    if (inRate != outRate) {
        if (soxr) {
            av_opt_set_int(s, "resampler", SWR_ENGINE_SOXR, 0);
            av_opt_set_int(s, "precision", 28, 0);   // "very high quality"
        } else {
            av_opt_set_int(s, "filter_size", 128, 0);
            av_opt_set_int(s, "phase_shift", 12, 0);
            av_opt_set_double(s, "cutoff", 0.97, 0);
        }
    }
    if (dither) av_opt_set_int(s, "dither_method", SWR_DITHER_TRIANGULAR_HIGHPASS, 0);
    if (swr_init(s) < 0) {
        swr_free(&s);
        return nullptr;
    }
    return s;
}

bool AudioRecorder::start(const std::string& path, const RecordOptions& o, std::string& err) {
    stop();
    opt_ = o;
    path_ = path;
    const bool mp3 = o.format == RecordOptions::MP3;
    outRate_ = o.outRate > 0 ? o.outRate : o.inRate;
    if (mp3 && outRate_ != 44100 && outRate_ != 48000 && outRate_ != 32000) outRate_ = 48000;   // límite del MP3
    const int bits = mp3 ? 16 : (o.bits == 24 ? 24 : 16);
    bytes_ = 0;
    samples_ = 0;
    {
        std::lock_guard<std::mutex> l(em_);
        err_.clear();
    }
    impl_ = new Impl;
    Impl& I = *impl_;
    auto fail = [&](const std::string& e) {
        err = std::string(formatName()) + ": " + e;
        delete impl_;
        impl_ = nullptr;
        return false;
    };

    // Formato que sale del convertidor y entra al codificador
    AVSampleFormat swrFmt, encFmt;
    if (mp3) {
        swrFmt = AV_SAMPLE_FMT_FLT;   // float intercalado; se separa por canales al montar el frame
        encFmt = AV_SAMPLE_FMT_FLTP;
        I.frameBytes = 8;
        I.planar = true;
    } else {
        swrFmt = encFmt = bits == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
        I.frameBytes = bits == 16 ? 4 : 8;
    }
    // dither sólo si se reduce resolución de verdad (fuente > 16 bits o
    // remuestreo); con fuente de 16 bits tal cual: copia exacta
    const bool dither = !mp3 && bits == 16 && (o.inBits > 16 || o.inRate != outRate_);
    if (o.inRate != outRate_) {
        I.swr = makeSwr(o.inRate, outRate_, swrFmt, dither, true);
        if (I.swr) LOG("%s: remuestreo %d -> %d Hz con SoX (máxima calidad)", formatName(), o.inRate, outRate_);
        else {
            I.swr = makeSwr(o.inRate, outRate_, swrFmt, dither, false);
            if (I.swr) LOG("%s: remuestreo %d -> %d Hz con swresample (filtro largo)", formatName(), o.inRate, outRate_);
        }
    } else {
        I.swr = makeSwr(o.inRate, outRate_, swrFmt, dither, false);
    }
    if (!I.swr) return fail("no se pudo crear el convertidor de audio");

    const AVCodec* c = mp3 ? avcodec_find_encoder_by_name("libmp3lame") : avcodec_find_encoder(AV_CODEC_ID_FLAC);
    if (!c) return fail(mp3 ? "FFmpeg sin libmp3lame" : "FFmpeg sin codificador FLAC");
    I.enc = avcodec_alloc_context3(c);
    I.enc->sample_fmt = encFmt;
    I.enc->sample_rate = outRate_;
    AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&I.enc->ch_layout, &st);
    I.enc->time_base = {1, outRate_};
    if (mp3) {
        if (o.mp3Kbps > 0) {
            I.enc->bit_rate = (int64_t)o.mp3Kbps * 1000;   // CBR
        } else {
            I.enc->flags |= AV_CODEC_FLAG_QSCALE;          // VBR V0 (la máxima calidad VBR de LAME)
            I.enc->global_quality = 0;
        }
        I.enc->compression_level = 0;   // algoritmo de máxima calidad de LAME (-q 0)
    } else {
        I.enc->bits_per_raw_sample = bits;
        I.enc->compression_level = 5;
    }
    int r = avcodec_open2(I.enc, c, nullptr);
    if (r < 0) return fail(averr(r));

    r = avformat_alloc_output_context2(&I.oc, nullptr, mp3 ? "mp3" : "flac", path.c_str());
    if (r < 0 || !I.oc) return fail("no se pudo crear " + path + ": " + averr(r));
    I.st = avformat_new_stream(I.oc, nullptr);
    avcodec_parameters_from_context(I.st->codecpar, I.enc);
    I.st->time_base = {1, outRate_};
    {
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char title[64], date[32];
        strftime(title, sizeof title, "Sesión DJ %Y-%m-%d %H:%M", &tmv);
        strftime(date, sizeof date, "%Y-%m-%d", &tmv);
        av_dict_set(&I.oc->metadata, "title", title, 0);
        av_dict_set(&I.oc->metadata, "date", date, 0);
        av_dict_set(&I.oc->metadata, "encoder", "DJ Live Streamer", 0);
    }
    r = avio_open(&I.oc->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (r < 0) return fail("no se pudo crear " + path + ": " + averr(r));
    r = avformat_write_header(I.oc, nullptr);
    if (r < 0) return fail(averr(r));

    I.fr = av_frame_alloc();
    I.fr->nb_samples = I.enc->frame_size > 0 ? I.enc->frame_size : 4608;
    I.fr->format = encFmt;
    I.fr->sample_rate = outRate_;
    av_channel_layout_copy(&I.fr->ch_layout, &st);
    av_frame_get_buffer(I.fr, 0);
    I.pkt = av_packet_alloc();

    stop_ = false;
    q_.clear();
    active_ = true;
    thr_ = std::thread(&AudioRecorder::loop, this);
    if (mp3) {
        std::string q = o.mp3Kbps > 0 ? std::to_string(o.mp3Kbps) + " kbps CBR" : std::string("VBR V0");
        LOG("grabando %s (MP3 %d Hz %s)", path.c_str(), outRate_, q.c_str());
    } else {
        LOG("grabando %s (FLAC %d Hz %d bits%s)", path.c_str(), outRate_, bits, dither ? ", con dither" : "");
    }
    return true;
}

void AudioRecorder::push(const float* in, int frames) {
    if (!active_) return;
    std::lock_guard<std::mutex> l(m_);
    if (stop_) return;
    // si el disco se atasca más de 10 minutos se pierde audio (pero nunca se bloquea la captura)
    if (q_.size() > 60000) {
        std::lock_guard<std::mutex> le(em_);
        if (err_.empty()) err_ = "el disco no da abasto: se pierde audio";
        return;
    }
    q_.emplace_back(in, in + (size_t)frames * 2);
    cv_.notify_one();
}

void AudioRecorder::stop() {
    if (!thr_.joinable()) return;
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    thr_.join();
    active_ = false;
    delete impl_;
    impl_ = nullptr;
    LOG("grabación cerrada: %s (%.1f MB, %.0f s)", path_.c_str(), bytes_ / 1048576.0, seconds());
}

std::string AudioRecorder::error() {
    std::lock_guard<std::mutex> l(em_);
    return err_;
}

bool AudioRecorder::writeFrames(bool flush) {
    Impl& I = *impl_;
    const int fs = I.fr->nb_samples;
    size_t off = 0;
    auto encode = [&](AVFrame* f) {
        if (avcodec_send_frame(I.enc, f) < 0) return false;
        while (avcodec_receive_packet(I.enc, I.pkt) >= 0) {
            av_packet_rescale_ts(I.pkt, I.enc->time_base, I.st->time_base);
            I.pkt->stream_index = 0;
            const int sz = I.pkt->size;
            int r = av_write_frame(I.oc, I.pkt);
            av_packet_unref(I.pkt);
            if (r < 0) {
                std::lock_guard<std::mutex> l(em_);
                err_ = "error escribiendo en disco: " + averr(r);
                LOG("%s: %s", formatName(), err_.c_str());
                return false;
            }
            bytes_ += (uint64_t)sz;
        }
        return true;
    };
    while (!I.failed) {
        size_t avail = (I.fifo.size() - off) / I.frameBytes;
        if (avail == 0 || (avail < (size_t)fs && !flush)) break;
        int n = (int)std::min<size_t>(avail, (size_t)fs);
        if (av_frame_make_writable(I.fr) < 0) break;
        I.fr->nb_samples = n;   // la última puede ser más corta (lo admite FLAC)
        if (I.planar) {
            // float intercalado -> un plano por canal
            const float* src = (const float*)(I.fifo.data() + off);
            float* L = (float*)I.fr->data[0];
            float* R = (float*)I.fr->data[1];
            for (int k = 0; k < n; k++) {
                L[k] = std::clamp(src[2 * k], -1.f, 1.f);   // por encima de 0 dBFS ya está saturado
                R[k] = std::clamp(src[2 * k + 1], -1.f, 1.f);
            }
        } else {
            memcpy(I.fr->data[0], I.fifo.data() + off, (size_t)n * I.frameBytes);
        }
        off += (size_t)n * I.frameBytes;
        I.fr->pts = I.pts;
        I.pts += n;
        samples_ += (uint64_t)n;
        if (!encode(I.fr)) I.failed = true;
        I.fr->nb_samples = fs;
    }
    I.fifo.erase(I.fifo.begin(), I.fifo.begin() + (ptrdiff_t)off);
    if (flush && !I.failed) {
        encode(nullptr);
        av_write_trailer(I.oc);   // FLAC: duración y MD5; MP3: cabecera Xing/LAME (duración, sin huecos)
    }
    return !I.failed;
}

void AudioRecorder::loop() {
    Impl& I = *impl_;
    uint64_t lastFlush = nowUs();
    while (true) {
        std::deque<std::vector<float>> work;
        bool stopping;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stop_ || !q_.empty(); });
            work.swap(q_);
            stopping = stop_;
        }
        for (auto& b : work) {
            int frames = (int)(b.size() / 2);
            int maxOut = swr_get_out_samples(I.swr, frames);
            if (maxOut <= 0) continue;
            I.tmp.resize((size_t)maxOut * I.frameBytes);
            uint8_t* o[1] = {I.tmp.data()};
            const uint8_t* i[1] = {(const uint8_t*)b.data()};
            int n = swr_convert(I.swr, o, maxOut, i, frames);
            if (n > 0) I.fifo.insert(I.fifo.end(), I.tmp.begin(), I.tmp.begin() + (ptrdiff_t)n * I.frameBytes);
        }
        if (stopping) {
            // lo que quede dentro del remuestreador
            int maxOut = swr_get_out_samples(I.swr, 0);
            if (maxOut > 0) {
                I.tmp.resize((size_t)maxOut * I.frameBytes);
                uint8_t* o[1] = {I.tmp.data()};
                int n = swr_convert(I.swr, o, maxOut, nullptr, 0);
                if (n > 0) I.fifo.insert(I.fifo.end(), I.tmp.begin(), I.tmp.begin() + (ptrdiff_t)n * I.frameBytes);
            }
            writeFrames(true);
            break;
        }
        writeFrames(false);
        if (nowUs() - lastFlush > 2000000) {   // a disco cada 2 s: si se cuelga el PC se pierde poco
            avio_flush(I.oc->pb);
            lastFlush = nowUs();
        }
    }
}
