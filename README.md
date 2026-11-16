# DJ Live Streamer

*[Versión en español más abajo](#dj-live-streamer-español)*

A desktop app (Qt) for live-streaming DJ sets to **Twitch and YouTube at the same time**, using the webcam as the picture and the **line input** as the sound, while **recording the audio to FLAC and/or MP3** on disk.

- Camera preview **not mirrored** (exactly as it goes out on the stream).
- Stereo **peak meter** (IEC scale, dBFS) with peak hold, max peak and a **SATURA** (clipping) light that stays on until you click it.
- Video encoded with **NVENC** (reused from gamecapture: direct NVENC with API 12.0, independent of the distro's FFmpeg). Falls back to x264 if there is no NVIDIA GPU.
- One encoder per distinct bitrate, so Twitch and YouTube can use different bitrates.
- If a destination drops, it **retries every 5 s** without affecting the other one or the recording. A keyframe is forced on reconnect so there is no waiting.
- Recording is independent of the stream. You can record **FLAC and MP3 at the same time** (same name: `djset_date.flac` + `djset_date.mp3`), or record without streaming: just untick Twitch and YouTube.
- MP3 uses LAME at its highest-quality setting: 320 kbps CBR (default), VBR V0, 256 or 192 kbps.

## Installation (Debian)

```sh
./install.sh --deps     # dependencies + build + install to /usr/local
```

It does not install any `nvidia-*` package: CUDA and NVENC are loaded from the driver you already have (driver >= 520).
Afterwards it shows up in the KDE menu as **DJ Live Streamer** (or run `djlivestreamer`).

## Usage

The interface is in Spanish; the original label is given in quotes, followed by its meaning.

1. **«Destinos»** (destinations): paste your Twitch and YouTube stream keys (the servers are already filled in).
2. **«Cámara y vídeo»** (camera and video): pick the camera and the mode, e.g. `1280×720 @ 60 fps (MJPEG)`. 60 fps is optional: if you pick a 30 fps mode, the stream runs at 30.
3. **«Audio y grabación»** (audio and recording): pick the line input. It shows the format the input runs at (e.g. `s32le, 2 canales, 48000 Hz`).
4. Set the level in KDE (input volume) while watching the meter: peaks below −3 dB, and the SATURA light should not come on.
5. Tick Twitch, YouTube, «Grabar FLAC» (record FLAC) and/or «Grabar MP3» (record MP3) and press **EMPEZAR** (start). To finish, press **TERMINAR** (stop): it asks for confirmation so a stray click can't end the set.

To test without going live: tick «Personalizado» (custom) and enter `~/Vídeos/prueba.flv` as the URL.

## About sample rates (44.1 / 48 kHz)

- **Twitch** accepts 44.1 and 48 kHz. **YouTube** recommends 44.1 kHz for stereo and also accepts 48 kHz.
- That's why the app **captures at your input's native rate** and uses that same rate for both the stream and the FLAC. **Nothing gets resampled** and no quality is lost.
- You can force 44.1 kHz for the FLAC (e.g. to make a CD). If the input runs at 48 kHz, it is then resampled with SoX at maximum quality (inaudible), but it is no longer an exact copy.
- With PipeWire, the input usually runs at 48 kHz. If you want it to really run at 44.1 kHz (without PipeWire resampling), add `default.clock.allowed-rates = [ 44100 48000 ]` in `~/.config/pipewire/pipewire.conf.d/` and set the sound card to 44.1 kHz in KDE. The app detects the rate and adapts on its own.
- **16 bits**: if the source has more resolution, triangular dither is added. If the source is already 16-bit and is not resampled, the copy is exact.
- The **MP3** uses the same rate as the FLAC. The MP3 format does not support more than 48 kHz: if the input runs at 96 kHz, the MP3 is recorded at 48 kHz.

## Sync

The sound card's clock is the master: the audio is never altered. Video timestamps are continuously adjusted to that clock, so audio and picture don't drift apart over sessions lasting hours.
If you notice a constant offset (usually the webcam lags a few milliseconds behind), correct it with «Sincronía imagen/sonido» (picture/sound sync): a positive value delays the sound.

## Files

- Recordings: `~/Música/Sesiones DJ/djset_YYYY-MM-DD_HH-MM-SS.flac` / `.mp3` (folder is configurable). Flushed to disk every 2 s.
- Settings (including the stream keys, with 600 permissions): `~/.config/djlivestreamer/djlivestreamer.conf`
- Log: `~/.local/state/djlivestreamer/djlivestreamer.log` (also in the «Registro» (log) tab).

## Testing without hardware

```sh
cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build
./build/pipeline_test 20 48000 150   # 20 s, 48 kHz input, sound card clock 150 ppm fast
```

It writes to `/tmp/djt/` two FLV files (one of them "connects" after 4 s, to test reconnection), two FLAC files and two MP3 files.

## Layout

| File | What it does |
|---|---|
| `src/camera.*` | V4L2 (YUYV/MJPEG), mode list, crop to 16:9 and conversion to BGRA with swscale |
| `src/audio.*` | PulseAudio/PipeWire capture at the native rate, input list, peak and clipping measurement |
| `src/encoder.*` | Direct NVENC (from gamecapture) and x264 fallback |
| `src/streamer.*` | AAC, clock adjustment and RTMP destinations with reconnection |
| `src/audiorecorder.*` | FLAC / MP3 recording, each in its own thread |
| `src/widgets.*` | Preview and level meter |
| `src/mainwindow.*` | User interface |

---

# DJ Live Streamer (español)

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
