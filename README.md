# DJ Live Streamer

Programa con interfaz gráfica (Qt) para emitir en directo sesiones de DJ a **Twitch y YouTube a la vez** con la webcam como imagen y la **entrada de línea** como sonido, y que **graba el audio en FLAC y/o MP3** en el disco al mismo tiempo.

- Vista previa de la cámara **sin espejo** (tal cual sale en el directo).
- **Vúmetro** estéreo de picos (escala IEC, dBFS) con retención, pico máximo y piloto **SATURA** que se queda encendido hasta que haces clic sobre él.
- Vídeo por **NVENC** (reaprovechado de gamecapture: NVENC directo con la API 12.0, sin depender del FFmpeg de la distro). Si no hay GPU NVIDIA, usa x264.
- Un codificador por cada tasa de bits distinta: Twitch y YouTube pueden ir con bitrates diferentes.
- Si se corta un destino se **reintenta cada 5 s** sin afectar al otro ni a la grabación. Al reconectar se fuerza un keyframe para no esperar.
- La grabación es independiente del directo. Puedes grabar **FLAC y MP3 a la vez** (mismo nombre, `djset_fecha.flac` + `djset_fecha.mp3`), o grabar sin emitir: desmarca Twitch y YouTube.
- MP3 con LAME en su modo de máxima calidad: 320 kbps CBR (por defecto), VBR V0, 256 o 192 kbps.

## Instalación (Debian)

```sh
./install.sh --deps     # dependencias + compilar + instalar en /usr/local
```

No instala ningún paquete `nvidia-*`: CUDA y NVENC se cargan del driver que ya tienes (driver >= 520).
Luego aparece en el menú de KDE como **DJ Live Streamer** (o ejecuta `djlivestreamer`).

## Uso

1. **Destinos**: pega las claves de emisión de Twitch y YouTube (los servidores ya vienen puestos).
2. **Cámara y vídeo**: elige la cámara y el modo, por ejemplo `1280×720 @ 60 fps (MJPEG)`. Los 60 fps son opcionales: si eliges un modo de 30 fps, el directo va a 30.
3. **Audio y grabación**: elige la entrada de línea. Ahí verás en qué formato trabaja (por ejemplo `s32le, 2 canales, 48000 Hz`).
4. Ajusta el nivel en KDE (volumen de la entrada) mirando el vúmetro: picos por debajo de −3 dB y que no se encienda SATURA.
5. Marca Twitch, YouTube, «Grabar FLAC» y/o «Grabar MP3» y pulsa **EMPEZAR**. Para terminar pulsa **TERMINAR**: pide confirmación, para que un clic sin querer no corte la sesión.

Para probar sin emitir: marca «Personalizado» y pon como URL `~/Vídeos/prueba.flv`.

## Sobre las frecuencias de muestreo (44,1 / 48 kHz)

- **Twitch** acepta 44,1 y 48 kHz. **YouTube** recomienda 44,1 kHz para estéreo y también acepta 48 kHz.
- Por eso el programa **captura a la frecuencia nativa de tu entrada** y usa esa misma frecuencia para el directo y para el FLAC. Así **no se remuestrea nada** y no se pierde calidad.
- En el FLAC puedes forzar 44,1 kHz (por ejemplo, para hacer un CD). Si la entrada va a 48 kHz, en ese caso se remuestrea con SoX en máxima calidad (inaudible), pero deja de ser una copia exacta.
- Con PipeWire, la entrada suele trabajar a 48 kHz. Si quieres que trabaje a 44,1 kHz de verdad (sin remuestreo de PipeWire), añade `default.clock.allowed-rates = [ 44100 48000 ]` en `~/.config/pipewire/pipewire.conf.d/` y, en KDE, pon la tarjeta a 44,1 kHz. El programa detecta la frecuencia y se adapta solo.
- **16 bits**: si la fuente tiene más resolución, se añade dither triangular. Si la fuente ya es de 16 bits y no se remuestrea, la copia es exacta.
- El **MP3** usa la misma frecuencia que el FLAC. El formato MP3 no admite más de 48 kHz: si la entrada va a 96 kHz, el MP3 se graba a 48 kHz.

## Sincronía

El reloj de la tarjeta de sonido manda: el audio nunca se toca. Las marcas de tiempo del vídeo se ajustan continuamente a ese reloj, así que en sesiones de horas el audio y la imagen no se separan.
Si notas un desfase fijo (normalmente la webcam va unos milisegundos por detrás), corrígelo en «Sincronía imagen/sonido»: con un valor positivo se retrasa el sonido.

## Archivos

- Grabaciones: `~/Música/Sesiones DJ/djset_AAAA-MM-DD_HH-MM-SS.flac` / `.mp3` (carpeta configurable). Se escriben a disco cada 2 s.
- Configuración (incluidas las claves, con permisos 600): `~/.config/djlivestreamer/djlivestreamer.conf`
- Registro: `~/.local/state/djlivestreamer/djlivestreamer.log` (también en la pestaña «Registro»).

## Prueba sin hardware

```sh
cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build
./build/pipeline_test 20 48000 150   # 20 s, entrada a 48 kHz, reloj de la tarjeta adelantado 150 ppm
```

Genera en `/tmp/djt/` dos FLV (uno de ellos «se conecta» a los 4 s, para probar la reconexión), dos FLAC y dos MP3.

## Estructura

| Archivo | Qué hace |
|---|---|
| `src/camera.*` | V4L2 (YUYV/MJPEG), lista de modos, recorte a 16:9 y conversión a BGRA con swscale |
| `src/audio.*` | Captura PulseAudio/PipeWire a la frecuencia nativa, lista de entradas y medición de picos y saturación |
| `src/encoder.*` | NVENC directo (de gamecapture) y x264 de respaldo |
| `src/streamer.*` | AAC, ajuste de reloj y destinos RTMP con reconexión |
| `src/audiorecorder.*` | Grabación FLAC / MP3, cada una en su propio hilo |
| `src/widgets.*` | Vista previa y vúmetro |
| `src/mainwindow.*` | Interfaz |
