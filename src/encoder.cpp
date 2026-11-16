#include "encoder.h"

#include <dlfcn.h>

#include <cstring>
#include <mutex>
#include <type_traits>

#include "dynlink_cuda.h"   // third_party/nvenc (cabeceras de NVIDIA, MIT)
#include "log.h"
#include "nvEncodeAPI.h"    // API NVENC 12.0: vale para cualquier driver >= 520

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// ---------------------------------------------------------------------------
// CUDA cargado con dlopen (viene con el driver NVIDIA, no hace falta el toolkit)
// ---------------------------------------------------------------------------
namespace {

struct Cuda {
    void* lib = nullptr;
    tcuInit* cuInit = nullptr;
    tcuDeviceGetCount* cuDeviceGetCount = nullptr;
    tcuDeviceGet* cuDeviceGet = nullptr;
    tcuDevicePrimaryCtxRetain* cuDevicePrimaryCtxRetain = nullptr;
    tcuCtxPushCurrent_v2* cuCtxPushCurrent = nullptr;
    tcuCtxPopCurrent_v2* cuCtxPopCurrent = nullptr;
    tcuMemAllocPitch_v2* cuMemAllocPitch = nullptr;
    tcuMemFree_v2* cuMemFree = nullptr;
    tcuMemcpy2DAsync_v2* cuMemcpy2DAsync = nullptr;
    tcuStreamSynchronize* cuStreamSynchronize = nullptr;
    tcuGetErrorName* cuGetErrorName = nullptr;
    CUcontext ctx = nullptr;
    CUdevice dev = 0;
    std::mutex m;

    const char* err(CUresult r) {
        const char* n = nullptr;
        if (cuGetErrorName) cuGetErrorName(r, &n);
        return n ? n : "?";
    }

    bool ensure(std::string& e) {
        std::lock_guard<std::mutex> l(m);
        if (ctx) return true;
        if (!lib) {
            lib = dlopen("libcuda.so.1", RTLD_NOW);
            if (!lib) { e = "no se pudo cargar libcuda.so.1 (¿driver NVIDIA?)"; return false; }
            bool ok = true;
            auto sym = [&](auto& fn, const char* name) {
                fn = (std::remove_reference_t<decltype(fn)>)dlsym(lib, name);
                if (!fn) ok = false;
            };
            sym(cuInit, "cuInit");
            sym(cuDeviceGetCount, "cuDeviceGetCount");
            sym(cuDeviceGet, "cuDeviceGet");
            sym(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
            sym(cuCtxPushCurrent, "cuCtxPushCurrent_v2");
            sym(cuCtxPopCurrent, "cuCtxPopCurrent_v2");
            sym(cuMemAllocPitch, "cuMemAllocPitch_v2");
            sym(cuMemFree, "cuMemFree_v2");
            sym(cuMemcpy2DAsync, "cuMemcpy2DAsync_v2");
            sym(cuStreamSynchronize, "cuStreamSynchronize");
            sym(cuGetErrorName, "cuGetErrorName");
            if (!ok) { dlclose(lib); lib = nullptr; e = "libcuda incompleta"; return false; }
        }
        CUresult r = cuInit(0);
        if (r != CUDA_SUCCESS) { e = std::string("cuInit: ") + err(r); return false; }
        int n = 0;
        cuDeviceGetCount(&n);
        if (n < 1) { e = "no hay GPU NVIDIA visible por CUDA"; return false; }
        cuDeviceGet(&dev, 0);
        if ((r = cuDevicePrimaryCtxRetain(&ctx, dev)) != CUDA_SUCCESS) {
            ctx = nullptr;
            e = std::string("contexto CUDA: ") + err(r);
            return false;
        }
        return true;
    }
    void push() { cuCtxPushCurrent(ctx); }
    void pop() { CUcontext d; cuCtxPopCurrent(&d); }
};
Cuda g_cuda;

// ---------------------------------------------------------------------------
// NVENC directo (sin el codificador de FFmpeg): se compila con la API 12.0 y se
// negocia con el driver al arrancar, así no depende de con qué versión de
// nv-codec-headers se compiló el FFmpeg de la distro.
// ---------------------------------------------------------------------------
class NvEncoder : public VideoEncoder {
public:
    ~NvEncoder() override { close(); }

    bool open(const VideoEncoderParams& p, std::string& e) {
        p_ = p;
        if (!g_cuda.ensure(e)) return false;
        lib_ = dlopen("libnvidia-encode.so.1", RTLD_NOW);
        if (!lib_) { e = "no se pudo cargar libnvidia-encode.so.1"; return false; }
        auto getMax = (NVENCSTATUS(NVENCAPI*)(uint32_t*))dlsym(lib_, "NvEncodeAPIGetMaxSupportedVersion");
        auto create = (NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*))dlsym(lib_, "NvEncodeAPICreateInstance");
        if (!getMax || !create) { e = "libnvidia-encode incompleta"; return false; }
        uint32_t maxv = 0;
        getMax(&maxv);
        const uint32_t need = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (maxv < need) { e = "driver NVIDIA demasiado antiguo para NVENC 12.0 (hace falta >= 520)"; return false; }
        fl_ = {};
        fl_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (create(&fl_) != NV_ENC_SUCCESS) { e = "NvEncodeAPICreateInstance falló"; return false; }
        g_cuda.push();
        bool ok = setup(e);
        g_cuda.pop();
        return ok;
    }

    std::string name() const override { return "NVENC H.264 (" + p_.preset + ")"; }

    bool encode(const uint8_t* bgra, int stride, int64_t ptsUs, std::vector<uint8_t>& out, bool& key) override {
        out.clear();
        g_cuda.push();
        // CPU -> VRAM (3,7 MB a 720p: nada para el PCIe)
        CUDA_MEMCPY2D cp{};
        cp.srcMemoryType = CU_MEMORYTYPE_HOST;
        cp.srcHost = bgra;
        cp.srcPitch = (size_t)stride;
        cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        cp.dstDevice = buf_;
        cp.dstPitch = pitch_;
        cp.WidthInBytes = (size_t)p_.w * 4;
        cp.Height = (size_t)p_.h;
        CUresult r = g_cuda.cuMemcpy2DAsync(&cp, nullptr);
        if (r == CUDA_SUCCESS) r = g_cuda.cuStreamSynchronize(nullptr);
        if (r != CUDA_SUCCESS) {
            LOG("NVENC: copia a la GPU: %s", g_cuda.err(r));
            g_cuda.pop();
            return false;
        }
        bool ok = false;
        NV_ENC_MAP_INPUT_RESOURCE mr{};
        mr.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mr.registeredResource = reg_;
        if (check(fl_.nvEncMapInputResource(enc_, &mr), "map")) {
            NV_ENC_PIC_PARAMS pp{};
            pp.version = NV_ENC_PIC_PARAMS_VER;
            pp.inputWidth = (uint32_t)p_.w;
            pp.inputHeight = (uint32_t)p_.h;
            pp.inputPitch = (uint32_t)pitch_;
            pp.inputBuffer = mr.mappedResource;
            pp.bufferFmt = mr.mappedBufferFmt;
            pp.outputBitstream = bs_;
            pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            pp.inputTimeStamp = (uint64_t)ptsUs;
            pp.frameIdx = frame_;
            if (frame_ == 0 || forceKey_.exchange(false))
                pp.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            frame_++;
            NVENCSTATUS st = fl_.nvEncEncodePicture(enc_, &pp);
            if (st == NV_ENC_SUCCESS) {
                NV_ENC_LOCK_BITSTREAM lb{};
                lb.version = NV_ENC_LOCK_BITSTREAM_VER;
                lb.outputBitstream = bs_;
                if (check(fl_.nvEncLockBitstream(enc_, &lb), "lock")) {
                    out.assign((uint8_t*)lb.bitstreamBufferPtr, (uint8_t*)lb.bitstreamBufferPtr + lb.bitstreamSizeInBytes);
                    key = lb.pictureType == NV_ENC_PIC_TYPE_IDR || lb.pictureType == NV_ENC_PIC_TYPE_I;
                    fl_.nvEncUnlockBitstream(enc_, bs_);
                    ok = true;
                }
            } else if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
                ok = true;
            } else {
                check(st, "codificar");
            }
            fl_.nvEncUnmapInputResource(enc_, mr.mappedResource);
        }
        g_cuda.pop();
        return ok;
    }

private:
    void close() {
        if (enc_) {
            g_cuda.push();
            if (reg_) fl_.nvEncUnregisterResource(enc_, reg_);
            if (bs_) fl_.nvEncDestroyBitstreamBuffer(enc_, bs_);
            fl_.nvEncDestroyEncoder(enc_);
            if (buf_) g_cuda.cuMemFree(buf_);
            g_cuda.pop();
        }
        enc_ = nullptr;
        reg_ = nullptr;
        bs_ = nullptr;
        buf_ = 0;
    }

    static GUID presetGuid(const std::string& p) {
        if (p == "p1") return NV_ENC_PRESET_P1_GUID;
        if (p == "p2") return NV_ENC_PRESET_P2_GUID;
        if (p == "p3") return NV_ENC_PRESET_P3_GUID;
        if (p == "p4") return NV_ENC_PRESET_P4_GUID;
        if (p == "p6") return NV_ENC_PRESET_P6_GUID;
        if (p == "p7") return NV_ENC_PRESET_P7_GUID;
        return NV_ENC_PRESET_P5_GUID;
    }

    bool check(NVENCSTATUS st, const char* what) {
        if (st == NV_ENC_SUCCESS) return true;
        const char* msg = enc_ && fl_.nvEncGetLastErrorString ? fl_.nvEncGetLastErrorString(enc_) : "";
        lastErr_ = std::string("NVENC ") + what + ": error " + std::to_string((int)st) + " " + (msg ? msg : "");
        LOG("%s", lastErr_.c_str());
        return false;
    }

    bool setup(std::string& e) {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{};
        op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        op.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        op.device = g_cuda.ctx;
        op.apiVersion = NVENCAPI_VERSION;
        if (!check(fl_.nvEncOpenEncodeSessionEx(&op, &enc_), "abrir sesión")) { enc_ = nullptr; e = lastErr_; return false; }

        const GUID codec = NV_ENC_CODEC_H264_GUID;
        const GUID preset = presetGuid(p_.preset);
        // Calidad alta: la latencia de unos pocos frames da igual en un directo por RTMP
        const NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;
        NV_ENC_PRESET_CONFIG pc{};
        pc.version = NV_ENC_PRESET_CONFIG_VER;
        pc.presetCfg.version = NV_ENC_CONFIG_VER;
        if (!check(fl_.nvEncGetEncodePresetConfigEx(enc_, codec, preset, tuning, &pc), "preset")) { e = lastErr_; return false; }
        NV_ENC_CONFIG c = pc.presetCfg;
        const uint32_t gop = (uint32_t)(p_.fps * (p_.keyintSec > 0 ? p_.keyintSec : 2));
        const uint32_t bps = (uint32_t)p_.kbps * 1000;
        c.gopLength = gop;
        c.frameIntervalP = 1;   // sin B-frames: un frame entra, un paquete sale
        c.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;   // Twitch y YouTube piden CBR
        c.rcParams.averageBitRate = bps;
        c.rcParams.maxBitRate = bps;
        c.rcParams.vbvBufferSize = bps;
        c.rcParams.vbvInitialDelay = bps;
        c.rcParams.enableLookahead = 0;
        c.rcParams.zeroReorderDelay = 1;
        c.rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
        c.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        auto& h = c.encodeCodecConfig.h264Config;
        h.idrPeriod = gop;
        h.repeatSPSPPS = 1;
        // NVENC convierte BGRA->YUV con matriz BT.601 en rango limitado
        auto& v = h.h264VUIParameters;
        v.videoSignalTypePresentFlag = 1;
        v.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
        v.videoFullRangeFlag = 0;
        v.colourDescriptionPresentFlag = 1;
        v.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        v.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        v.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT470BG;

        NV_ENC_INITIALIZE_PARAMS ip{};
        ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
        ip.encodeGUID = codec;
        ip.presetGUID = preset;
        ip.encodeWidth = ip.darWidth = ip.maxEncodeWidth = (uint32_t)p_.w;
        ip.encodeHeight = ip.darHeight = ip.maxEncodeHeight = (uint32_t)p_.h;
        ip.frameRateNum = (uint32_t)p_.fps;
        ip.frameRateDen = 1;
        ip.enablePTD = 1;
        ip.encodeConfig = &c;
        ip.tuningInfo = tuning;
        if (!check(fl_.nvEncInitializeEncoder(enc_, &ip), "inicializar")) { e = lastErr_; return false; }

        NV_ENC_CREATE_BITSTREAM_BUFFER cb{};
        cb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (!check(fl_.nvEncCreateBitstreamBuffer(enc_, &cb), "bitstream")) { e = lastErr_; return false; }
        bs_ = cb.bitstreamBuffer;

        CUresult r = g_cuda.cuMemAllocPitch(&buf_, &pitch_, (size_t)p_.w * 4, (size_t)p_.h, 16);
        if (r != CUDA_SUCCESS) { buf_ = 0; e = std::string("cuMemAllocPitch: ") + g_cuda.err(r); return false; }

        NV_ENC_REGISTER_RESOURCE rr{};
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        rr.width = (uint32_t)p_.w;
        rr.height = (uint32_t)p_.h;
        rr.pitch = (uint32_t)pitch_;
        rr.resourceToRegister = (void*)buf_;
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;   // en memoria: B,G,R,A
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (!check(fl_.nvEncRegisterResource(enc_, &rr), "registrar superficie")) { e = lastErr_; return false; }
        reg_ = rr.registeredResource;

        uint8_t tmp[1024];
        uint32_t sz = 0;
        NV_ENC_SEQUENCE_PARAM_PAYLOAD sp{};
        sp.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
        sp.inBufferSize = sizeof tmp;
        sp.spsppsBuffer = tmp;
        sp.outSPSPPSPayloadSize = &sz;
        if (!check(fl_.nvEncGetSequenceParams(enc_, &sp), "cabecera SPS/PPS")) { e = lastErr_; return false; }
        header.assign(tmp, tmp + sz);
        return true;
    }

    VideoEncoderParams p_;
    void* lib_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST fl_{};
    void* enc_ = nullptr;
    NV_ENC_OUTPUT_PTR bs_ = nullptr;
    NV_ENC_REGISTERED_PTR reg_ = nullptr;
    CUdeviceptr buf_ = 0;
    size_t pitch_ = 0;
    uint32_t frame_ = 0;
    std::string lastErr_;
};

// ---------------------------------------------------------------------------
// x264 por software (si no hay NVENC)
// ---------------------------------------------------------------------------
class X264Encoder : public VideoEncoder {
public:
    ~X264Encoder() override {
        sws_freeContext(sws_);
        av_frame_free(&fr_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
    }

    bool open(const VideoEncoderParams& p, std::string& e) {
        p_ = p;
        const AVCodec* c = avcodec_find_encoder_by_name("libx264");
        if (!c) { e = "FFmpeg sin libx264"; return false; }
        ctx_ = avcodec_alloc_context3(c);
        ctx_->width = p.w;
        ctx_->height = p.h;
        ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx_->time_base = {1, 1000000};
        ctx_->framerate = {p.fps, 1};
        ctx_->bit_rate = (int64_t)p.kbps * 1000;
        ctx_->rc_max_rate = ctx_->bit_rate;
        ctx_->rc_buffer_size = (int)ctx_->bit_rate;
        ctx_->gop_size = p.fps * (p.keyintSec > 0 ? p.keyintSec : 2);
        ctx_->keyint_min = ctx_->gop_size;
        ctx_->max_b_frames = 0;
        ctx_->thread_count = 0;
        ctx_->colorspace = AVCOL_SPC_BT470BG;
        ctx_->color_range = AVCOL_RANGE_MPEG;
        ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set(ctx_->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx_->priv_data, "profile", "high", 0);
        av_opt_set(ctx_->priv_data, "forced-idr", "1", 0);
        av_opt_set(ctx_->priv_data, "x264-params", "nal-hrd=cbr:force-cfr=0:scenecut=0", 0);
        int r = avcodec_open2(ctx_, c, nullptr);
        if (r < 0) { e = "libx264: " + averr(r); return false; }
        header.assign(ctx_->extradata, ctx_->extradata + ctx_->extradata_size);
        fr_ = av_frame_alloc();
        fr_->format = AV_PIX_FMT_YUV420P;
        fr_->width = p.w;
        fr_->height = p.h;
        av_frame_get_buffer(fr_, 0);
        pkt_ = av_packet_alloc();
        sws_ = sws_getContext(p.w, p.h, AV_PIX_FMT_BGRA, p.w, p.h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) { e = "swscale"; return false; }
        return true;
    }

    std::string name() const override { return "x264 (CPU, veryfast)"; }

    bool encode(const uint8_t* bgra, int stride, int64_t ptsUs, std::vector<uint8_t>& out, bool& key) override {
        out.clear();
        if (av_frame_make_writable(fr_) < 0) return false;
        const uint8_t* src[1] = {bgra};
        int ss[1] = {stride};
        sws_scale(sws_, src, ss, 0, p_.h, fr_->data, fr_->linesize);
        fr_->pts = ptsUs;
        fr_->pict_type = forceKey_.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (avcodec_send_frame(ctx_, fr_) < 0) return false;
        int r = avcodec_receive_packet(ctx_, pkt_);
        if (r == AVERROR(EAGAIN)) return true;
        if (r < 0) return false;
        out.assign(pkt_->data, pkt_->data + pkt_->size);
        key = pkt_->flags & AV_PKT_FLAG_KEY;
        av_packet_unref(pkt_);
        return true;
    }

private:
    VideoEncoderParams p_;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* fr_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;
};

}   // namespace

std::unique_ptr<VideoEncoder> createVideoEncoder(const std::string& kind, const VideoEncoderParams& p, std::string& err) {
    if (kind != "x264") {
        auto e = std::make_unique<NvEncoder>();
        std::string why;
        if (e->open(p, why)) return e;
        if (kind == "nvenc") { err = why; return nullptr; }
        LOG("NVENC no disponible (%s): se usa x264 por CPU", why.c_str());
    }
    auto x = std::make_unique<X264Encoder>();
    if (x->open(p, err)) return x;
    return nullptr;
}
