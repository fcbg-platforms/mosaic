# Compiler warnings and hardening flags, applied via a reusable interface target.
# Link target_link_libraries(your_target PRIVATE mosaic_compiler_options)

add_library(mosaic_compiler_options INTERFACE)

if(MSVC)
    target_compile_options(mosaic_compiler_options INTERFACE
        /W4 /WX           # High warnings, treat as errors
        /permissive-      # Strict conformance
        /EHsc             # Standard C++ exception model — CMake's own MSVC
                           # default-flags injection adds this automatically
                           # for the Visual Studio generator, but not when
                           # CMAKE_CXX_COMPILER=cl is set explicitly under
                           # -G Ninja (used by the clang-tidy CI job's
                           # compile_commands.json build) — made explicit so
                           # the exception model doesn't silently depend on
                           # which generator happens to be configuring.
        /utf-8            # Source/execution charset
        /Zc:__cplusplus   # Report correct __cplusplus value
        # Release still ships debug info (/Zi + linker /DEBUG below) — purely
        # additive, no effect on codegen/optimization/behavior — so a crash
        # dump from a real Release build (the only config ever run outside
        # this dev machine) can actually be symbolized instead of only
        # showing raw module+offset. /FS avoids PDB-write contention under
        # /MP's parallel compilation.
        $<$<CONFIG:Release>:/Zi /FS>
    )
    if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
        # /MP (parallel compilation within one cl.exe invocation, across
        # every .cpp in the target) only makes sense for MSBuild-driven
        # builds (the multi-config Visual Studio generator — this project's
        # real production build, via build-and-test in CI). Ninja already
        # parallelizes at the build-graph level (one cl.exe process per
        # translation unit), so /MP is not just redundant there but actively
        # rejected: clang (as used by the clang-tidy CI job analyzing a
        # Ninja-generated compile_commands.json) treats "argument unused
        # during compilation" as a hard error, which would otherwise make
        # every single file fail tidy analysis before any check even runs.
        target_compile_options(mosaic_compiler_options INTERFACE /MP)
    endif()
    target_link_options(mosaic_compiler_options INTERFACE
        # /OPT:REF,ICF re-asserted explicitly alongside /DEBUG so enabling
        # symbols doesn't accidentally change what Release already strips —
        # MSVC defaults these ON for Release, but that default is only
        # implicit until /DEBUG is added, so pin it rather than assume it.
        $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mosaic_compiler_options INTERFACE
        -Wall -Wextra -Wpedantic -Werror
        -Wno-unused-parameter
    )
endif()
