// Prueba sin cámara ni tarjeta: vídeo y audio sintéticos en tiempo real a
// Streamer (a ficheros, uno de ellos "desconectado" al principio) y a FlacRecorder.
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "audiorecorder.h"
#include "log.h"
#include "streamer.h"

int main(int argc, char** argv) {
    logInit();
    const int secs = argc > 1 ? atoi(argv[1]) : 12;
    const int rate = argc > 2 ? atoi(argv[2]) : 48000;
    const double ppm = argc > 3 ? atof(argv[3]) : 0;   // reloj de la tarjeta adelantado
    if (system("rm -rf /tmp/djt && mkdir -p /tmp/djt")) {}
    Streamer st;
    StreamConfig c;
    c.dests = {{"A", "/tmp/djt/a.flv", 3000}, {"B", "/tmp/djt/late/b.flv", 2000}};
    c.width = 1280; c.height = 720; c.fps = 60; c.inputRate = rate; c.audioKbps = 160;
    std::string err;
    if (!st.start(c, err)) { printf("start: %s\n", err.c_str()); return 1; }
    AudioRecorder f1, f2, f3, f4;
    RecordOptions o;
    o.inRate = rate;
    o.bits = 24; o.inBits = 24;
    if (!f1.start("/tmp/djt/native24.flac", o, err)) { printf("%s\n", err.c_str()); return 1; }
    o.bits = 16; o.outRate = 44100;
    if (!f2.start("/tmp/djt/cd.flac", o, err)) { printf("%s\n", err.c_str()); return 1; }
    o.format = RecordOptions::MP3; o.outRate = 0; o.mp3Kbps = 320;
    if (!f3.start("/tmp/djt/cbr320.mp3", o, err)) { printf("%s\n", err.c_str()); return 1; }
    o.mp3Kbps = 0;
    if (!f4.start("/tmp/djt/v0.mp3", o, err)) { printf("%s\n", err.c_str()); return 1; }
    std::vector<uint8_t> img(1280 * 720 * 4);
    const int blk = rate / 100;
    std::vector<float> a(blk * 2);
    uint64_t t0 = nowUs(), nextV = t0, nextA = t0 + 10000;
    int64_t n = 0;
    int fr = 0;
    double accA = 0;
    bool made = false;
    while (nowUs() - t0 < (uint64_t)secs * 1000000) {
        uint64_t now = nowUs();
        if (now >= nextA) {
            for (int i = 0; i < blk; i++, n++) {
                float s = 0.5f * sinf(2 * M_PI * 1000.0 * n / rate);
                a[2 * i] = s; a[2 * i + 1] = -s;
            }
            st.pushAudio(a.data(), blk, (int64_t)nextA - 10000);
            f1.push(a.data(), blk); f2.push(a.data(), blk); f3.push(a.data(), blk); f4.push(a.data(), blk);
            nextA += (uint64_t)llround(10000.0 / (1 + ppm * 1e-6) + accA);
            accA = 0;
        }
        if (now >= nextV) {
            for (int y = 0; y < 720; y++)
                for (int x = 0; x < 1280; x++) {
                    uint8_t* p = &img[(y * 1280 + x) * 4];
                    bool bar = ((x + fr * 8) % 1280) < 80;
                    p[0] = bar ? 255 : x / 5; p[1] = y / 3; p[2] = bar ? 255 : 80; p[3] = 255;
                }
            st.pushVideo(img.data(), 1280 * 4, 1280, 720, (int64_t)now);
            fr++;
            nextV += 16667;
        }
        if (!made && now - t0 > 4000000) { mkdir("/tmp/djt/late", 0755); made = true; }
        usleep(1000);
        if (fr % 120 == 0 && now >= nextV - 16667) {
            for (auto& s : st.status()) printf("  %s: %s %.0f kbps\n", s.name.c_str(), s.state.c_str(), s.kbps);
        }
    }
    printf("encode fps %.1f dropped %llu\n", st.encodeFps(), (unsigned long long)st.droppedFrames());
    st.stop(); f1.stop(); f2.stop(); f3.stop(); f4.stop();
    return 0;
}
