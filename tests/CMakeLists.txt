find_package(GTest REQUIRED)
# Network: needed for QPasswordDigestor, used by profile_manager.cpp
# (PBKDF2 password hashing) — lives in QtNetwork, not QtCore.
find_package(Qt6 REQUIRED COMPONENTS Core Network)

add_executable(mosaic_tests
    test_ring_buffer.cpp
    test_timestamp.cpp
    test_sync_manifest.cpp
    test_camera_persistence.cpp
    test_param_mapping.cpp
    test_gige_action_command.cpp
    test_pose_analysis_result.cpp
    test_pose_kinematics.cpp
    test_transcript_result.cpp
    test_expression_result.cpp
    test_rms_quality.cpp
    test_room_frame_solver.cpp
    test_gaze_fusion_result.cpp
    "${CMAKE_SOURCE_DIR}/src/video/gige_action_command.cpp"
    "${CMAKE_SOURCE_DIR}/src/calibration/rms_quality.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/sync_manifest.cpp"
    "${CMAKE_SOURCE_DIR}/src/calibration/room_frame_solver.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze_fusion_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/pose_analysis_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/pose_kinematics.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/transcript_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/expression_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/auth/profile_manager.cpp"
    # Add new test files here as subsystems grow.
)

target_link_libraries(mosaic_tests PRIVATE
    GTest::gtest_main
    mosaic_compiler_options
    Qt6::Core       # needed for QString / QDateTime used in timestamp.hpp
    Qt6::Network    # needed for QPasswordDigestor used by profile_manager.cpp
)

target_include_directories(mosaic_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

include(GoogleTest)
gtest_discover_tests(mosaic_tests)
