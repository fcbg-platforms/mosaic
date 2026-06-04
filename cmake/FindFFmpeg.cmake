# Finds FFmpeg component libraries.
# Usage: find_package(FFmpeg REQUIRED COMPONENTS avcodec avformat avutil swscale swresample)
# Creates targets: FFmpeg::avcodec, FFmpeg::avformat, etc.

set(_FFmpeg_REQUIRED_VARS)

foreach(_component IN LISTS FFmpeg_FIND_COMPONENTS)
    string(TOUPPER "${_component}" _UPPER)

    find_path(FFMPEG_${_UPPER}_INCLUDE_DIR
        NAMES "lib${_component}/${_component}.h"
        HINTS ENV FFMPEG_ROOT
        PATH_SUFFIXES include
    )

    find_library(FFMPEG_${_UPPER}_LIBRARY
        NAMES "${_component}"
        HINTS ENV FFMPEG_ROOT
        PATH_SUFFIXES lib
    )

    list(APPEND _FFmpeg_REQUIRED_VARS FFMPEG_${_UPPER}_INCLUDE_DIR FFMPEG_${_UPPER}_LIBRARY)

    if(FFMPEG_${_UPPER}_INCLUDE_DIR AND FFMPEG_${_UPPER}_LIBRARY)
        set(FFmpeg_${_component}_FOUND TRUE)
        if(NOT TARGET FFmpeg::${_component})
            add_library(FFmpeg::${_component} SHARED IMPORTED)
            set_target_properties(FFmpeg::${_component} PROPERTIES
                IMPORTED_LOCATION "${FFMPEG_${_UPPER}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${_UPPER}_INCLUDE_DIR}"
            )
        endif()
    endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS ${_FFmpeg_REQUIRED_VARS}
    HANDLE_COMPONENTS
)
