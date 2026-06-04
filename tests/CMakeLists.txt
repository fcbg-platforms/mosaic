find_package(GTest REQUIRED)

add_executable(mosaic_tests
    test_ring_buffer.cpp
    test_timestamp.cpp
)

target_link_libraries(mosaic_tests PRIVATE
    GTest::gtest_main
    mosaic_compiler_options
)

include(GoogleTest)
gtest_discover_tests(mosaic_tests)
