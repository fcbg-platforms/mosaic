find_package(GTest REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(mosaic_tests
    test_ring_buffer.cpp
    test_timestamp.cpp
    # Add new test files here as subsystems grow.
)

target_link_libraries(mosaic_tests PRIVATE
    GTest::gtest_main
    mosaic_compiler_options
    Qt6::Core    # needed for QString / QDateTime used in timestamp.hpp
)

target_include_directories(mosaic_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

include(GoogleTest)
gtest_discover_tests(mosaic_tests)
