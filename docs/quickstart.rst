Quick start
===========

.. contents:: On this page
   :local:
   :depth: 2

Prerequisites
-------------

.. tab-set::

   .. tab-item:: macOS

      .. code-block:: bash

         # Qt 6.4+ via Homebrew
         brew install qt cmake

         # Optional extras
         brew install ffmpeg opencv   # for video encoding / calibration
         # liblsl from https://github.com/sccn/liblsl/releases

   .. tab-item:: Windows

      Install the following, in order:

      1. `Visual Studio 2022 <https://visualstudio.microsoft.com/>`_ — *Desktop development with C++* workload.
      2. `Qt 6.4+ <https://www.qt.io/download>`_ — choose the **msvc2022_64** kit.
         Note the install path (e.g. ``C:\Qt\6.8.1\msvc2022_64``).
      3. `vcpkg <https://github.com/microsoft/vcpkg>`_ — for GTest / FFmpeg / OpenCV.
      4. `Basler Pylon SDK 7.x <https://www.baslerweb.com/en/downloads/software-downloads/>`_ — only needed with real cameras.

Building
--------

.. tab-set::

   .. tab-item:: macOS

      .. code-block:: bash

         git clone https://github.com/your-org/mosaic.git
         cd mosaic

         # Debug build (cameras off, no FFmpeg) — works on any Mac
         ./scripts/configure.sh

         cmake --build build/Debug --parallel

         # With LSL + OpenCV calibration
         ./scripts/configure.sh Debug --lsl --opencv
         cmake --build build/Debug --parallel

   .. tab-item:: Windows

      .. code-block:: powershell

         git clone https://github.com/your-org/mosaic.git
         cd mosaic

         # Basic build (all hardware off, tests included)
         .\scripts\configure.ps1

         # Full build (cameras + NVENC + LSL + calibration)
         .\scripts\configure.ps1 -EnableCameras -EnableNvenc -EnableLsl -EnableOpenCV

         cmake --build build\Release --parallel

         # Deploy Qt DLLs so the .exe runs on other machines
         windeployqt --qmldir src\qml build\Release\bin\mosaic.exe

Feature flags
~~~~~~~~~~~~~

Pass these to CMake to enable optional subsystems:

.. list-table::
   :header-rows: 1
   :widths: 35 10 55

   * - Flag
     - Default
     - Requires
   * - ``MOSAIC_ENABLE_CAMERAS``
     - OFF
     - Basler Pylon SDK 7.x at ``%PYLON_ROOT%``
   * - ``MOSAIC_ENABLE_FFMPEG``
     - OFF
     - FFmpeg 4.x / 5.x at ``%FFMPEG_ROOT%``
   * - ``MOSAIC_ENABLE_NVENC``
     - OFF
     - FFMPEG + CUDA + NVIDIA driver
   * - ``MOSAIC_ENABLE_LSL``
     - OFF
     - liblsl (vcpkg feature ``lsl``)
   * - ``MOSAIC_ENABLE_OPENCV``
     - OFF
     - OpenCV 4.x
   * - ``MOSAIC_ENABLE_PARALLEL_PORT``
     - OFF
     - Windows + ``InpOut32.dll`` next to the exe
   * - ``MOSAIC_BUILD_TESTS``
     - OFF
     - GTest (via vcpkg)

.. tip::

   All features compile with **stub fallbacks** when disabled.  You can develop
   and test the full UI on a MacBook without any lab hardware.

First launch
------------

1. Run ``mosaic`` (or ``mosaic.app`` on macOS).
2. The **login dialog** appears. Click **"+ New profile"** to create your
   research group's profile (username, group name, optional password).
3. After logging in the main window opens. Use the tabs on the left to
   configure cameras, microphones, triggers, and the recording output folder.
4. Press **● Record** (or ``Ctrl+R``) to start a session. Files appear in
   the configured output folder.

Building the documentation
--------------------------

.. code-block:: bash

   pip install -r docs/requirements.txt

   # Build with CMake (recommended — runs Doxygen automatically)
   cmake -S . -B build/Debug -DMOSAIC_BUILD_DOCS=ON
   cmake --build build/Debug --target docs
   open build/Debug/docs/sphinx/html/index.html

   # Or build Sphinx standalone (requires Doxygen XML already generated)
   doxygen docs/Doxyfile.in
   cd docs
   DOXYGEN_XML=../build/Debug/doxygen/xml \
       sphinx-build -b html . _build/html
   open _build/html/index.html

Running the tests
-----------------

.. code-block:: bash

   cmake -S . -B build/Debug -DMOSAIC_BUILD_TESTS=ON
   cmake --build build/Debug --parallel
   cd build/Debug && ctest --output-on-failure
