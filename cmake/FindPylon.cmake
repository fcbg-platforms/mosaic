# Finds the Basler Pylon SDK.
# Sets:  Pylon::Pylon (IMPORTED target)
# Hints: PYLON_ROOT env var or -DPYLON_ROOT=...

find_path(PYLON_INCLUDE_DIR
    NAMES pylon/PylonIncludes.h
    HINTS
        ENV PYLON_ROOT
        ENV PYLON_DEV_DIR
        "C:/Program Files/Basler/pylon 7/Development"
    PATH_SUFFIXES include
)

find_library(PYLON_IMPLIB
    # Basler's PylonBase ABI-version suffix has changed across SDK releases
    # independently of the "pylon 7" product name (e.g. pylon 7.4.x shipped
    # PylonBase_v7_4, while pylon 25.06.x ships PylonBase_v10) — try the
    # known suffixes plus the unsuffixed name.
    NAMES PylonBase_v10 PylonBase_v9 PylonBase_v8 PylonBase_v7_4 PylonBase
    HINTS
        ENV PYLON_ROOT
        ENV PYLON_DEV_DIR
        "C:/Program Files/Basler/pylon 7/Development"
    PATH_SUFFIXES lib/x64 lib
)

# Derive the lib directory so #pragma comment(lib, ...) auto-links can resolve.
if(PYLON_IMPLIB)
    get_filename_component(PYLON_LIB_DIR "${PYLON_IMPLIB}" DIRECTORY)
endif()

find_file(PYLON_DLL
    NAMES PylonBase_v10.dll PylonBase_v9.dll PylonBase_v8.dll PylonBase_v7_4.dll PylonBase.dll
    HINTS
        ENV PYLON_ROOT
        "C:/Program Files/Basler/pylon 7/Runtime"
    PATH_SUFFIXES x64 bin
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Pylon DEFAULT_MSG PYLON_IMPLIB PYLON_INCLUDE_DIR)

if(Pylon_FOUND AND NOT TARGET Pylon::Pylon)
    add_library(Pylon::Pylon SHARED IMPORTED)

    if(MSVC AND PYLON_DLL)
        set_target_properties(Pylon::Pylon PROPERTIES
            IMPORTED_LOCATION             "${PYLON_DLL}"
            IMPORTED_IMPLIB               "${PYLON_IMPLIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${PYLON_INCLUDE_DIR}"
            INTERFACE_LINK_DIRECTORIES    "${PYLON_LIB_DIR}"
        )
    else()
        # MinGW / non-MSVC: use the .lib directly as IMPORTED_LOCATION
        set_target_properties(Pylon::Pylon PROPERTIES
            IMPORTED_LOCATION             "${PYLON_IMPLIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${PYLON_INCLUDE_DIR}"
            INTERFACE_LINK_DIRECTORIES    "${PYLON_LIB_DIR}"
        )
    endif()
endif()
