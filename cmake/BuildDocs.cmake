# BuildDocs.cmake — optional documentation targets.
# Included by the root CMakeLists.txt when MOSAIC_BUILD_DOCS=ON.
#
# Targets added:
#   docs_doxygen  — runs Doxygen, produces HTML + XML in <binary_dir>/doxygen/
#   docs_sphinx   — runs Sphinx, depends on docs_doxygen (requires Python + pip packages)
#   docs          — alias that runs both

find_package(Doxygen OPTIONAL_COMPONENTS dot)

if(NOT DOXYGEN_FOUND)
    message(WARNING "[docs] Doxygen not found — docs_doxygen target will not be available.")
    return()
endif()

# ── Doxygen ────────────────────────────────────────────────────────────────

configure_file(
    "${CMAKE_SOURCE_DIR}/docs/Doxyfile.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    @ONLY
)

add_custom_target(docs_doxygen
    COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running Doxygen..."
    VERBATIM
)

# ── Sphinx ─────────────────────────────────────────────────────────────────

find_package(Python3 COMPONENTS Interpreter)

if(NOT Python3_FOUND)
    message(STATUS "[docs] Python 3 not found — docs_sphinx target will not be available.")
else()
    # Check that at least sphinx-build is installed.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m sphinx --version
        OUTPUT_QUIET ERROR_QUIET
        RESULT_VARIABLE _sphinx_result
    )
    if(NOT _sphinx_result EQUAL 0)
        message(STATUS
            "[docs] sphinx not installed — run 'pip install -r docs/requirements.txt'.")
    else()
        set(_sphinx_html "${CMAKE_CURRENT_BINARY_DIR}/docs/sphinx/html")

        add_custom_target(docs_sphinx
            COMMAND "${CMAKE_COMMAND}" -E env
                    "DOXYGEN_XML=${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml"
                    "${Python3_EXECUTABLE}" -m sphinx
                    -b html
                    -E                          # always rebuild (no cache)
                    "${CMAKE_SOURCE_DIR}/docs"
                    "${_sphinx_html}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            DEPENDS docs_doxygen
            COMMENT "Running Sphinx (output: ${_sphinx_html}/index.html)..."
            VERBATIM
        )
    endif()
endif()

# ── Convenience alias ──────────────────────────────────────────────────────

if(TARGET docs_sphinx)
    add_custom_target(docs DEPENDS docs_sphinx)
elseif(TARGET docs_doxygen)
    add_custom_target(docs DEPENDS docs_doxygen)
    message(STATUS "[docs] 'make docs' will run Doxygen only (Sphinx not available).")
endif()
