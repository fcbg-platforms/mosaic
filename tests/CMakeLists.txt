find_package(GTest REQUIRED)
# Network: needed for QPasswordDigestor, used by profile_manager.cpp
# (PBKDF2 password hashing) — lives in QtNetwork, not QtCore.
find_package(Qt6 REQUIRED COMPONENTS Core Network)

# Hermetic stand-in for the real Python interpreter, used only by
# test_analysis_manager.cpp (via AnalysisManager::set_python_path()) — see
# analysis_stub/main.cpp's own doc comment. Deliberately zero Qt
# dependency, built as a plain sibling executable in the same output
# directory as mosaic_tests itself.
add_executable(analysis_test_stub analysis_stub/main.cpp)

add_executable(mosaic_tests
    test_main.cpp
    test_ring_buffer.cpp
    test_timestamp.cpp
    test_sync_manifest.cpp
    test_trigger_frame_map.cpp
    test_trigger_recorder.cpp
    test_trigger_persistence.cpp
    test_camera_persistence.cpp
    test_record_persistence.cpp
    test_param_mapping.cpp
    test_fps_readout.cpp
    test_gige_action_command.cpp
    test_pose_analysis_result.cpp
    test_pose_kinematics.cpp
    test_dyadic_kinematics.cpp
    test_transcript_result.cpp
    test_expression_result.cpp
    test_rms_quality.cpp
    test_room_frame_solver.cpp
    test_gaze_fusion_result.cpp
    test_skeleton3d_result.cpp
    test_rppg_result.cpp
    test_gaze2d_result.cpp
    test_sync_repair_result.cpp
    test_audio_envelope.cpp
    test_audio_persistence.cpp
    test_realtime_metrics.cpp
    test_speaker_palette.cpp
    test_recording_access_control.cpp
    test_dpapi_crypt.cpp
    test_analysis_manager.cpp
    test_session_health.cpp
    "${CMAKE_SOURCE_DIR}/src/session/session_health.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/recording_access_control.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/audio/speaker_palette.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/fps_readout.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/gige_action_command.cpp"
    "${CMAKE_SOURCE_DIR}/src/calibration/rms_quality.cpp"
    "${CMAKE_SOURCE_DIR}/src/audio/audio_envelope.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/sync_manifest.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/trigger_frame_map.cpp"
    "${CMAKE_SOURCE_DIR}/src/trigger/trigger_recorder.cpp"
    "${CMAKE_SOURCE_DIR}/src/calibration/room_frame_solver.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze_fusion_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/skeleton3d_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/rppg_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze2d_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/sync_repair_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/pose_analysis_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/pose_kinematics.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/dyadic_kinematics.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/transcript_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/expression_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/realtime_metrics.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/analysis_manager.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/dpapi_crypt.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/auth/profile_manager.cpp"
    # Add new test files here as subsystems grow.
)

# So `cmake --build . --target mosaic_tests` alone still produces the stub
# it needs at runtime, without requiring a separate "build everything" pass.
add_dependencies(mosaic_tests analysis_test_stub)

target_link_libraries(mosaic_tests PRIVATE
    GTest::gtest    # was GTest::gtest_main — test_main.cpp now provides main()
                    # itself, so QCoreApplication exists for QProcess's async
                    # signals (see test_analysis_manager.cpp's wait_for_signal()).
    mosaic_compiler_options
    Qt6::Core       # needed for QString / QDateTime used in timestamp.hpp
    Qt6::Network    # needed for QPasswordDigestor used by profile_manager.cpp
)

if(WIN32)
    target_link_libraries(mosaic_tests PRIVATE Crypt32)  # utils/dpapi_crypt.cpp
endif()

target_include_directories(mosaic_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

include(GoogleTest)
gtest_discover_tests(mosaic_tests)
