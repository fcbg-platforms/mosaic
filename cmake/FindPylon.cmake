# Finds the Basler Pylon SDK.
# Sets:  Pylon::Pylon (IMPORTED target)
# Hints: PYLON_ROOT env var or -DPYLON_ROOT=...

find_path(PYLON_INCLUDE_DIR
    NAMES pylon/PylonIncludes.h
    HINTS
        ENV PYLON_ROOT
        "C:/Program Files/Basler/pylon 7/Development"
    PATH_SUFFIXES include
)

find_library(PYLON_LIBRARY
    NAMES PylonBase_v7_4 PylonBase
    HINTS
        ENV PYLON_ROOT
        "C:/Program Files/Basler/pylon 7/Development"
    PATH_SUFFIXES lib/x64 lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Pylon DEFAULT_MSG PYLON_LIBRARY PYLON_INCLUDE_DIR)

if(Pylon_FOUND AND NOT TARGET Pylon::Pylon)
    add_library(Pylon::Pylon SHARED IMPORTED)
    set_target_properties(Pylon::Pylon PROPERTIES
        IMPORTED_LOCATION "${PYLON_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PYLON_INCLUDE_DIR}"
    )
endif()
