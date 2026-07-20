# Compiler warnings and hardening flags, applied via a reusable interface target.
# Link target_link_libraries(your_target PRIVATE mosaic_compiler_options)

add_library(mosaic_compiler_options INTERFACE)

if(MSVC)
    target_compile_options(mosaic_compiler_options INTERFACE
        /W4 /WX           # High warnings, treat as errors
        /permissive-      # Strict conformance
        /MP               # Parallel compilation
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
