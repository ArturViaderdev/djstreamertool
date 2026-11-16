// Registro: a stderr, a ~/.local/state/djlivestreamer/djlivestreamer.log y a
// la ventana (pestaña "Registro").
#pragma once
#include <cstdint>
#include <functional>
#include <string>

void logInit();
std::string logPath();
void logMsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// La interfaz se registra aquí para recibir cada línea (se llama desde cualquier hilo)
void logSetSink(std::function<void(const std::string&)> f);
#define LOG(...) logMsg(__VA_ARGS__)

uint64_t nowUs();             // CLOCK_MONOTONIC, el mismo reloj que las marcas de V4L2
std::string averr(int e);     // texto de un error de FFmpeg
