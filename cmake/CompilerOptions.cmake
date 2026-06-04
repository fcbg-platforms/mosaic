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
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mosaic_compiler_options INTERFACE
        -Wall -Wextra -Wpedantic -Werror
        -Wno-unused-parameter
    )
endif()
