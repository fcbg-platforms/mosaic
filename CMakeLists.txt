cmake_minimum_required(VERSION 3.25)

project(mosaic
    VERSION 0.1.0
    DESCRIPTION "Multi-camera Observatory for Social & Activity Interaction Capture"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(cmake/CompilerOptions.cmake)

# Optional: vcpkg integration
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS
    Core Gui Widgets Network Multimedia
    Quick QuickWidgets
)
find_package(Qt6 OPTIONAL_COMPONENTS SerialPort)
qt_standard_project_setup()

option(MOSAIC_ENABLE_SERIAL "Build serial port trigger support" ON)
if(MOSAIC_ENABLE_SERIAL AND TARGET Qt6::SerialPort)
    set(MOSAIC_HAVE_SERIAL ON)
else()
    set(MOSAIC_HAVE_SERIAL OFF)
endif()

# ── Feature flags ─────────────────────────────────────────────────────────
option(MOSAIC_ENABLE_CAMERAS       "Build Basler Pylon camera support"         OFF)
option(MOSAIC_ENABLE_FFMPEG        "Build FFmpeg video encoding"               OFF)
option(MOSAIC_ENABLE_NVENC         "Build NVIDIA NVENC GPU encoder (implies FFmpeg)" OFF)
option(MOSAIC_ENABLE_PARALLEL_PORT "Build InpOut32 parallel port trigger"      OFF)
option(MOSAIC_ENABLE_LSL           "Build Lab Streaming Layer support"         OFF)
option(MOSAIC_ENABLE_OPENCV        "Build OpenCV calibration"                  OFF)

list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_SOURCE_DIR}/cmake")

# ── Cameras (Basler Pylon) ─────────────────────────────────────────────────
if(MOSAIC_ENABLE_CAMERAS)
    find_package(Pylon REQUIRED)
endif()

# ── FFmpeg / NVENC ─────────────────────────────────────────────────────────
if(MOSAIC_ENABLE_NVENC)
    set(MOSAIC_ENABLE_FFMPEG ON CACHE BOOL "" FORCE)
endif()

if(MOSAIC_ENABLE_FFMPEG)
    find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
endif()

# ── OpenCV ─────────────────────────────────────────────────────────────────
if(MOSAIC_ENABLE_OPENCV)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d)
endif()

# ── Lab Streaming Layer ────────────────────────────────────────────────────
if(MOSAIC_ENABLE_LSL)
    find_package(LSL REQUIRED)
endif()

add_subdirectory(src)

option(MOSAIC_BUILD_TESTS "Build unit tests" OFF)
option(MOSAIC_BUILD_DOCS  "Build Doxygen + Sphinx documentation" OFF)
if(MOSAIC_BUILD_DOCS)
    include(cmake/BuildDocs.cmake)
endif()
if(MOSAIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
