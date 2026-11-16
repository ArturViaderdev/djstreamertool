#include "log.h"

#include <sys/stat.h>
#include <time.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

static FILE* g_file = nullptr;
static std::mutex g_m;
static std::function<void(const std::string&)> g_sink;

std::string logPath() {
    std::string base;
    if (const char* x = getenv("XDG_STATE_HOME"); x && *x) base = x;
    else if (const char* h = getenv("HOME")) base = std::string(h) + "/.local/state";
    else base = "/tmp";
    return base + "/djlivestreamer/djlivestreamer.log";
}

static void mkdirs(std::string p) {
    for (size_t i = 1; i < p.size(); i++)
        if (p[i] == '/') {
            p[i] = 0;
            mkdir(p.c_str(), 0755);
            p[i] = '/';
        }
    mkdir(p.c_str(), 0755);
}

static void writeLine(const std::string& line, bool toGui) {
    std::lock_guard<std::mutex> l(g_m);
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s] %s\n", ts, line.c_str());
    if (g_file) {
        fprintf(g_file, "[%s] %s\n", ts, line.c_str());
        fflush(g_file);
    }
    if (toGui && g_sink) g_sink(std::string(ts) + "  " + line);
}

// Errores internos de FFmpeg: sólo al fichero (los avisos del decodificador
// MJPEG de algunas webcams llenarían la ventana)
static void ffLogCb(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_ERROR) return;
    char line[1024];
    int pp = 1;
    av_log_format_line(avcl, level, fmt, vl, line, sizeof line, &pp);
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    writeLine(std::string("ffmpeg: ") + line, false);
}

void logInit() {
    std::string p = logPath();
    mkdirs(p.substr(0, p.rfind('/')));
    g_file = fopen(p.c_str(), "a");
    av_log_set_callback(ffLogCb);
}

void logSetSink(std::function<void(const std::string&)> f) {
    std::lock_guard<std::mutex> l(g_m);
    g_sink = std::move(f);
}

void logMsg(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    writeLine(buf, true);
}

uint64_t nowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

std::string averr(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(e, b, sizeof b);
    return b;
}
