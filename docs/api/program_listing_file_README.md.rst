
.. _program_listing_file_README.md:

Program Listing for File README.md
==================================

|exhale_lsh| :ref:`Return to documentation for file <file_README.md>` (``README.md``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: markdown

   # MOSAIC
   
   **Multi-camera Observatory for Social & Activity Interaction Capture**
   
   A professional, cross-platform recording suite for research labs working on:
   - Pose estimation
   - Social interaction analysis
   - Behavioral coding
   - Multi-modal data acquisition
   
   Supports synchronized capture from multiple Basler cameras, microphone arrays, and hardware trigger sources.
   
   ---
   
   ## Requirements
   
   | Tool | Version |
   |---|---|
   | CMake | ≥ 3.25 |
   | C++ compiler | MSVC 2022 / GCC 13 / Clang 17 |
   | Qt | 6.4+ |
   | Pylon SDK | 7.x |
   | FFmpeg | 4.x / 5.x |
   | OpenCV | 4.8 |
   | vcpkg | (optional, for GTest / OpenCV / FFmpeg) |
   
   ---
   
   ## Building
   
   ```powershell
   # 1. Clone
   git clone https://github.com/your-org/mosaic.git
   cd mosaic
   
   # 2. Install vcpkg packages (optional)
   vcpkg install
   
   # 3. Configure & build
   .\scripts\configure.ps1 -BuildType Release
   cmake --build build/Release --parallel
   ```
   
   Run tests:
   ```powershell
   cd build/Release
   ctest --output-on-failure
   ```
   
   ---
   
   ## Project structure
   
   ```
   mosaic/
   ├── cmake/          # Find modules and compiler options
   ├── src/
   │   ├── core/       # Application, settings
   │   ├── video/      # Camera grabber, encoder
   │   ├── audio/      # Audio recorder
   │   ├── trigger/    # Keyboard / serial / parallel triggers
   │   ├── ui/         # Qt widgets and windows
   │   └── utils/      # Logger, ring buffer, timestamps
   ├── tests/          # Google Test unit tests
   ├── docs/           # Doxygen config
   ├── resources/      # Icons, QSS stylesheets
   └── scripts/        # Build helper scripts
   ```
   
   ---
   
   ## License
   
   MIT
