cmake_minimum_required(VERSION 3.25)

project(mosaic
    VERSION 0.1.0
    DESCRIPTION "Multi-camera Observatory for Social & Activity Interaction Capture"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output binaries to a predictable location
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Compiler options module
include(cmake/CompilerOptions.cmake)

# Optional: vcpkg integration (set VCPKG_ROOT env or pass -DCMAKE_TOOLCHAIN_FILE)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# Find Qt (version pinned to major only; test with Qt 6.x)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network SerialPort Multimedia)
qt_standard_project_setup()

# Third-party: Pylon (Basler cameras)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(Pylon REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
find_package(OpenCV REQUIRED COMPONENTS core imgproc)

add_subdirectory(src)

# Tests — only build when explicitly requested
option(MOSAIC_BUILD_TESTS "Build unit tests" OFF)
if(MOSAIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
