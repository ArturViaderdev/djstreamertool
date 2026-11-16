#!/bin/sh
# Compila e instala DJ Live Streamer en /usr/local.
#   ./install.sh          -> compila e instala (pide sudo para instalar)
#   ./install.sh --deps   -> instala antes las dependencias de Debian/Ubuntu
set -e
cd "$(dirname "$0")"

if [ "$1" = "--deps" ]; then
    # Ojo: no instala ningún paquete nvidia-* (CUDA y NVENC vienen con tu driver)
    sudo apt-get install -y build-essential cmake ninja-build pkg-config qt6-base-dev \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
        libpulse0 pulseaudio-utils
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
# --clean-first: si se copian fuentes con fechas antiguas ninja no lo detectaría
cmake --build build --clean-first
sudo cmake --install build
echo
echo "Listo: búscalo en el menú de KDE como «DJ Live Streamer» o ejecuta: djlivestreamer"
