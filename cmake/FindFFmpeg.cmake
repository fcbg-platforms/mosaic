# Finds FFmpeg component libraries.
# Usage: find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
# Creates targets: FFmpeg::avcodec, FFmpeg::avformat, etc.
#
# Searches FFMPEG_ROOT (CMake var or env var) first.
# On Windows: sets IMPORTED_IMPLIB to the .lib file; locates the matching .dll
# for IMPORTED_LOCATION so downstream code can copy it at install time.

set(_FFmpeg_REQUIRED_VARS)

foreach(_component IN LISTS FFmpeg_FIND_COMPONENTS)
    string(TOUPPER "${_component}" _UPPER)

    find_path(FFMPEG_${_UPPER}_INCLUDE_DIR
        NAMES "lib${_component}/${_component}.h"
        HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT
        PATH_SUFFIXES include
    )

    find_library(FFMPEG_${_UPPER}_LIBRARY
        NAMES "${_component}"
        HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT
        PATH_SUFFIXES lib
    )

    list(APPEND _FFmpeg_REQUIRED_VARS FFMPEG_${_UPPER}_INCLUDE_DIR FFMPEG_${_UPPER}_LIBRARY)

    if(FFMPEG_${_UPPER}_INCLUDE_DIR AND FFMPEG_${_UPPER}_LIBRARY)
        set(FFmpeg_${_component}_FOUND TRUE)
        if(NOT TARGET FFmpeg::${_component})
            add_library(FFmpeg::${_component} SHARED IMPORTED)

            if(WIN32)
                # On Windows the .lib is the import library; look for the matching DLL.
                find_file(_FFMPEG_${_UPPER}_DLL
                    NAMES "${_component}-61.dll" "${_component}-60.dll"
                          "${_component}-59.dll" "${_component}-58.dll"
                          "${_component}.dll"
                    HINTS "${FFMPEG_ROOT}" ENV FFMPEG_ROOT
                    PATH_SUFFIXES bin
                )
                set_target_properties(FFmpeg::${_component} PROPERTIES
                    IMPORTED_IMPLIB   "${FFMPEG_${_UPPER}_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${_UPPER}_INCLUDE_DIR}"
                )
                if(_FFMPEG_${_UPPER}_DLL)
                    set_target_properties(FFmpeg::${_component} PROPERTIES
                        IMPORTED_LOCATION "${_FFMPEG_${_UPPER}_DLL}"
                    )
                else()
                    # No DLL found — still usable for linking if the DLL is on PATH at runtime.
                    set_target_properties(FFmpeg::${_component} PROPERTIES
                        IMPORTED_LOCATION "${FFMPEG_${_UPPER}_LIBRARY}"
                    )
                endif()
                unset(_FFMPEG_${_UPPER}_DLL CACHE)
            else()
                set_target_properties(FFmpeg::${_component} PROPERTIES
                    IMPORTED_LOCATION "${FFMPEG_${_UPPER}_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${_UPPER}_INCLUDE_DIR}"
                )
            endif()
        endif()
    endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS ${_FFmpeg_REQUIRED_VARS}
    HANDLE_COMPONENTS
)
