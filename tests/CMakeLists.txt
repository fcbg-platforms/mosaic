find_package(GTest REQUIRED)
# Network: needed for QPasswordDigestor, used by profile_manager.cpp
# (PBKDF2 password hashing) — lives in QtNetwork, not QtCore.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)

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
    test_camera_timestamp_files.cpp
    test_camera_label.cpp
    test_interview_mode.cpp
    test_trigger_frame_map.cpp
    test_trigger_recorder.cpp
    test_trigger_persistence.cpp
    test_camera_persistence.cpp
    test_record_persistence.cpp
    test_param_mapping.cpp
    test_fps_readout.cpp
    test_gige_action_command.cpp
    test_action_tick_log.cpp
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
    test_face_dynamics_result.cpp
    test_conversation_result.cpp
    test_session_overview.cpp
    test_eye_contact_result.cpp
    test_social_result.cpp
    test_objects_result.cpp
    test_hands_result.cpp
    test_chart_data.cpp
    test_golden_session.cpp
    test_sync_repair_result.cpp
    test_audio_envelope.cpp
    test_audio_persistence.cpp
    test_realtime_metrics.cpp
    test_speaker_palette.cpp
    test_recording_access_control.cpp
    test_dpapi_crypt.cpp
    test_analysis_manager.cpp
    test_session_health.cpp
    test_camera_health.cpp
    test_gige_bandwidth.cpp
    test_frame_shortfall.cpp
    test_session_name.cpp
    test_preflight.cpp
    test_session_archive.cpp
    test_problem_journal.cpp
    test_data_root.cpp
    test_pc_settings.cpp
    test_crash_safety.cpp
    test_time_axis.cpp
    test_spectrogram_colormap.cpp
    test_voice_result.cpp
    test_analysis_plugins.cpp
    "${CMAKE_SOURCE_DIR}/src/session/session_end.cpp"
    "${CMAKE_SOURCE_DIR}/src/session/session_archive.cpp"
    "${CMAKE_SOURCE_DIR}/src/session/session_health.cpp"
    "${CMAKE_SOURCE_DIR}/src/session/preflight.cpp"
    "${CMAKE_SOURCE_DIR}/src/audio/wav_writer.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/frame_timestamp_writer.cpp"
    "${CMAKE_SOURCE_DIR}/src/session/session_name.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/recording_access_control.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/data_root.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/pc_settings.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/audio/speaker_palette.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/audio/spectrogram_colormap.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/fps_readout.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/frame_shortfall.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/gige_action_command.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/gige_bandwidth.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/action_tick_log.cpp"
    "${CMAKE_SOURCE_DIR}/src/calibration/rms_quality.cpp"
    "${CMAKE_SOURCE_DIR}/src/audio/audio_envelope.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/camera_timestamp_files.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/sync_manifest.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/trigger_frame_map.cpp"
    "${CMAKE_SOURCE_DIR}/src/trigger/trigger_recorder.cpp"
    "${CMAKE_SOURCE_DIR}/src/calibration/room_frame_solver.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze_fusion_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze_summary.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze_pose_match.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/pose3d_people.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/skeleton3d_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/analysis/room3d_scene.cpp" # the 3D room view's maths
    "${CMAKE_SOURCE_DIR}/src/ui/analysis/room3d_stage.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/analysis/gaze3d_scene.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/rppg_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze2d_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/face_dynamics_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/conversation_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/eye_contact_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/social_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/objects_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/hands_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/chart_data.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/session_overview.cpp"
    "${CMAKE_SOURCE_DIR}/src/audio/audio_timing_log.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/sync_repair_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/video/synced_video_export.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/pose_analysis_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/pose_kinematics.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/dyadic_kinematics.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/transcript_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/voice_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/expression_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/realtime_metrics.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/analysis_manager.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/logger.cpp"
    "${CMAKE_SOURCE_DIR}/src/utils/problem_journal.cpp"
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
    Qt6::Gui        # QColor/QVector3D/QQuaternion in the 3D room view's scene
)

if(WIN32)
    target_link_libraries(mosaic_tests PRIVATE Crypt32)  # utils/dpapi_crypt.cpp
endif()

# The calibration managers' board handling, tested on real board images
# rendered by OpenCV, so only when the build has it (as the app does).
if(MOSAIC_ENABLE_OPENCV)
    target_sources(mosaic_tests PRIVATE
        test_calibration_board_change.cpp
        "${CMAKE_SOURCE_DIR}/src/calibration/calibration_manager.cpp"
        "${CMAKE_SOURCE_DIR}/src/calibration/lens_model.cpp"
        test_lens_model.cpp
        "${CMAKE_SOURCE_DIR}/src/calibration/room_calibration_manager.cpp"
        "${CMAKE_SOURCE_DIR}/src/calibration/room_bundle_adjust.cpp"
        test_room_bundle_adjust.cpp
        "${CMAKE_SOURCE_DIR}/src/calibration/marked_chessboard.cpp"
        test_marked_chessboard.cpp
    )
    target_link_libraries(mosaic_tests PRIVATE
        Qt6::Gui  # CalibrationManager's QImage previews
        opencv_core opencv_imgproc opencv_calib3d opencv_objdetect)
    target_compile_definitions(mosaic_tests PRIVATE MOSAIC_HAVE_OPENCV)
endif()

# The golden analysis session (analysis/tests/golden_session.py), read in place.
target_compile_definitions(mosaic_tests PRIVATE
    MOSAIC_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/session")

target_include_directories(mosaic_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

include(GoogleTest)
# Listing the tests runs the program once; a busy CI runner can take longer
# than CMake's default 5 s for that, which failed the build (PR #122).
gtest_discover_tests(mosaic_tests DISCOVERY_TIMEOUT 60)

# The QML pages, loaded without a window (nothing drawn): a page's QML
# errors and binding warnings, which only show when it runs, fail the test.
# Its own program, since a page needs a QGuiApplication.
add_executable(mosaic_qml_tests
    test_qml_main.cpp
    test_room3d_page.cpp
    "${CMAKE_SOURCE_DIR}/src/analysis/skeleton3d_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze_fusion_result.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze_summary.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/gaze_pose_match.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/pose3d_people.cpp"
    "${CMAKE_SOURCE_DIR}/src/analysis/dyadic_kinematics.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/analysis/gaze3d_scene.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/analysis/room3d_scene.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/analysis/room3d_stage.cpp"
    "${CMAKE_SOURCE_DIR}/src/ui/analysis/room3d_mesh.cpp"
)
qt_add_resources(mosaic_qml_tests "qml_pages"
    PREFIX "/"
    BASE   "${CMAKE_SOURCE_DIR}/src"
    FILES
        "${CMAKE_SOURCE_DIR}/src/qml/Room3D.qml"
        "${CMAKE_SOURCE_DIR}/src/qml/Gaze3D.qml"
        "${CMAKE_SOURCE_DIR}/src/qml/RoomStage.qml"
        "${CMAKE_SOURCE_DIR}/src/qml/RoomPill.qml"
        "${CMAKE_SOURCE_DIR}/src/qml/RoomTag.qml"
        "${CMAKE_SOURCE_DIR}/src/qml/RoomBodies.qml"
)
target_link_libraries(mosaic_qml_tests PRIVATE
    GTest::gtest
    mosaic_compiler_options
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Quick3D
)
target_include_directories(mosaic_qml_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
gtest_discover_tests(mosaic_qml_tests DISCOVERY_TIMEOUT 60)
