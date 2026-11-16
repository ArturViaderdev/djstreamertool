cmake_minimum_required(VERSION 3.16)
project(djlivestreamer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
include(GNUInstallDirs)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FF REQUIRED IMPORTED_TARGET
  libavcodec libavformat libavutil libswscale libswresample)
find_package(Threads REQUIRED)

add_executable(djlivestreamer
  src/main.cpp src/mainwindow.cpp src/widgets.cpp
  src/camera.cpp src/audio.cpp src/encoder.cpp src/streamer.cpp
  src/audiorecorder.cpp src/log.cpp)
# CUDA, NVENC y PulseAudio se cargan con dlopen: sólo cabeceras en third_party
target_include_directories(djlivestreamer PRIVATE third_party/nvenc src)
target_link_libraries(djlivestreamer PRIVATE Qt${QT_VERSION_MAJOR}::Widgets PkgConfig::FF Threads::Threads dl)
target_compile_options(djlivestreamer PRIVATE -Wall -Wextra -Wno-unused-parameter)

install(TARGETS djlivestreamer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES djlivestreamer.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# Prueba sin cámara ni tarjeta de sonido (vídeo y audio sintéticos a /tmp/djt)
option(BUILD_TESTS "Compilar las pruebas" OFF)
if(BUILD_TESTS)
  add_executable(pipeline_test test/pipeline_test.cpp src/streamer.cpp src/encoder.cpp
                 src/audiorecorder.cpp src/log.cpp)
  target_include_directories(pipeline_test PRIVATE third_party/nvenc src)
  target_link_libraries(pipeline_test PRIVATE PkgConfig::FF Threads::Threads dl)
endif()
