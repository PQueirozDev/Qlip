cmake_minimum_required(VERSION 3.24)

project(Qlip VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(Qlip WIN32
    src/main.cpp
    src/Qlip.rc
    src/Qlip.manifest
    src/resource.h
    src/app/Application.cpp
    src/app/Application.h
    src/audio/AudioDevices.cpp
    src/audio/AudioDevices.h
    src/audio/AudioRing.h
    src/audio/AudioSource.cpp
    src/audio/AudioSource.h
    src/capture/Monitors.cpp
    src/capture/Monitors.h
    src/capture/ScreenCapture.cpp
    src/capture/ScreenCapture.h
    src/config/SettingsManager.cpp
    src/config/SettingsManager.h
    src/config/Startup.cpp
    src/config/Startup.h
    src/encoder/AudioEncoder.cpp
    src/encoder/AudioEncoder.h
    src/encoder/VideoEncoder.cpp
    src/encoder/VideoEncoder.h
    src/recording/RecordingManager.cpp
    src/recording/RecordingManager.h
    src/recording/VideoMuxer.cpp
    src/recording/VideoMuxer.h
    src/replay/ReplayBuffer.cpp
    src/replay/ReplayBuffer.h
    src/replay/ReplayManager.cpp
    src/replay/ReplayManager.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/SettingsWindow.cpp
    src/ui/SettingsWindow.h
    src/ui/LibraryWindow.cpp
    src/ui/LibraryWindow.h
    src/ui/Dropdown.cpp
    src/ui/Dropdown.h
    src/ui/OverlayWindow.cpp
    src/ui/OverlayWindow.h
    src/ui/Theme.cpp
    src/ui/Theme.h
    src/ui/TrayIcon.cpp
    src/ui/TrayIcon.h
    src/utils/BoundedQueue.h
    src/utils/Logger.cpp
    src/utils/Logger.h
    src/utils/Text.h
)

# FFmpeg LGPL shared (sem x264/x265): headers, import libs e DLLs em third_party/ffmpeg.
set(QCLIP_FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg)
target_include_directories(Qlip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# SYSTEM: avisos dos headers do FFmpeg não entram no nosso build.
target_include_directories(Qlip SYSTEM PRIVATE ${QCLIP_FFMPEG_DIR}/include)
target_link_directories(Qlip PRIVATE ${QCLIP_FFMPEG_DIR}/lib)

target_compile_definitions(Qlip PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(Qlip PRIVATE
    nlohmann_json::nlohmann_json
    avcodec avformat avutil swscale
    d3d11 dxgi windowsapp winmm
    shell32 user32 gdi32 gdiplus ole32 comctl32 uxtheme dwmapi shcore
)

if(MSVC)
    target_compile_options(Qlip PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_link_options(Qlip PRIVATE /SUBSYSTEM:WINDOWS)
    set_property(TARGET Qlip PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Copia as DLLs do FFmpeg para junto do executável (aplicativo portátil).
file(GLOB QCLIP_FFMPEG_DLLS ${QCLIP_FFMPEG_DIR}/bin/*.dll)
add_custom_command(TARGET Qlip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${QCLIP_FFMPEG_DLLS} $<TARGET_FILE_DIR:Qlip>
    COMMAND_EXPAND_LISTS
)
