# Small standalone diagnostics, built only when Pylon is available (mirrors
# how src/CMakeLists.txt links Pylon::Pylon for the mosaic target itself).

add_executable(camera_nodemap_probe camera_nodemap_probe.cpp)
target_link_libraries(camera_nodemap_probe PRIVATE Pylon::Pylon)
