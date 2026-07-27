qt_add_executable(mosaic MACOSX_BUNDLE
    main.cpp

    # ── Authentication / profile management ───────────────────────────────
    auth/profile.hpp
    auth/profile_manager.hpp
    auth/profile_manager.cpp
    ui/auth/login_dialog.hpp
    ui/auth/login_dialog.cpp
    ui/auth/admin_panel_dialog.hpp
    ui/auth/admin_panel_dialog.cpp

    # ── Analysis manager ──────────────────────────────────────────────────
    analysis/analysis_manager.hpp
    analysis/analysis_manager.cpp
    analysis/pose_worker.hpp
    analysis/pose_worker.cpp
    analysis/pose_models.hpp
    analysis/pose_analysis_result.hpp
    analysis/pose_analysis_result.cpp
    analysis/pose_kinematics.hpp
    analysis/pose_kinematics.cpp
    analysis/transcript_result.hpp
    analysis/transcript_result.cpp
    analysis/expression_result.hpp
    analysis/expression_result.cpp
    analysis/sync_manifest.hpp
    analysis/sync_manifest.cpp

    # ── Foundation ────────────────────────────────────────────────────────
    utils/logger.hpp
    utils/logger.cpp
    utils/timestamp.hpp
    utils/ring_buffer.hpp

    # ── Core ──────────────────────────────────────────────────────────────
    core/settings.hpp
    core/settings.cpp
    core/application.hpp
    core/application.cpp

    # ── UI chrome ─────────────────────────────────────────────────────────
    ui/style.hpp
    ui/anim_utils.hpp
    ui/anim_utils.cpp
    ui/main_window.hpp
    ui/main_window.cpp

    # ── Logger panel ──────────────────────────────────────────────────────
    ui/logger/logger_panel_w.hpp
    ui/logger/logger_panel_w.cpp

    # ── Video settings panel ──────────────────────────────────────────────
    ui/video/video_settings_w.hpp
    ui/video/video_settings_w.cpp
    ui/video/camera_card_w.hpp
    ui/video/camera_card_w.cpp
    ui/video/performance_monitor_w.hpp
    ui/video/performance_monitor_w.cpp

    # ── Audio settings panel ──────────────────────────────────────────────
    ui/audio/audio_settings_w.hpp
    ui/audio/audio_settings_w.cpp
    ui/audio/audio_waveform_w.hpp
    ui/audio/audio_waveform_w.cpp
    ui/audio/microphone_card_w.hpp
    ui/audio/microphone_card_w.cpp

    # ── Record settings panel ─────────────────────────────────────────────
    ui/record/record_settings_w.hpp
    ui/record/record_settings_w.cpp

    # ── Trigger backend ───────────────────────────────────────────────────
    trigger/trigger_types.hpp
    trigger/keyboard_trigger.hpp
    trigger/keyboard_trigger.cpp
    trigger/trigger_recorder.hpp
    trigger/trigger_recorder.cpp
    trigger/trigger_manager.hpp
    trigger/trigger_manager.cpp
    trigger/lsl_outlet.hpp
    trigger/lsl_outlet.cpp
    trigger/lsl_inlet.hpp
    trigger/lsl_inlet.cpp
    trigger/parallel_port_trigger.hpp
    trigger/parallel_port_trigger.cpp

    # ── Trigger settings panel ────────────────────────────────────────────
    ui/trigger/keyboard_card_w.hpp
    ui/trigger/keyboard_card_w.cpp
    ui/trigger/trigger_settings_w.hpp
    ui/trigger/trigger_settings_w.cpp
    ui/trigger/trigger_event_panel_w.hpp
    ui/trigger/trigger_event_panel_w.cpp
    ui/trigger/parallel_port_card_w.hpp
    ui/trigger/parallel_port_card_w.cpp

    # ── Audio backend ─────────────────────────────────────────────────────
    audio/wav_writer.hpp
    audio/wav_writer.cpp
    audio/audio_recorder.hpp
    audio/audio_recorder.cpp
    audio/audio_manager.hpp
    audio/audio_manager.cpp

    # ── Video backend ─────────────────────────────────────────────────────
    video/video_frame.hpp
    video/frame_timestamp_writer.hpp
    video/frame_timestamp_writer.cpp
    video/param_mapping.hpp
    video/gige_action_command.hpp
    video/gige_action_command.cpp
    video/video_grabber.hpp
    video/video_grabber.cpp
    video/video_encoder.hpp
    video/video_encoder.cpp
    video/video_manager.hpp
    video/video_manager.cpp
    video/video_feed_provider.hpp
    video/video_feed_provider.cpp

    # ── Calibration ───────────────────────────────────────────────────────
    calibration/calibration_manager.hpp
    calibration/calibration_manager.cpp
    calibration/rms_quality.hpp
    calibration/rms_quality.cpp
    ui/calibration/badge_style.hpp
    ui/calibration/badge_style.cpp
    ui/calibration/calibration_w.hpp
    ui/calibration/calibration_w.cpp

    # ── Record manager ────────────────────────────────────────────────────
    record/record_manager.hpp
    record/record_manager.cpp

    # ── Session browser & player ──────────────────────────────────────────
    session/session_info.hpp
    ui/session/session_browser_w.hpp
    ui/session/session_browser_w.cpp
    ui/session/session_player_w.hpp
    ui/session/session_player_w.cpp

    # ── Analysis tab ─────────────────────────────────────────────────────
    ui/analysis/pose_overlay_player_w.hpp
    ui/analysis/pose_overlay_player_w.cpp
    ui/analysis/analysis_tab_w.hpp
    ui/analysis/analysis_tab_w.cpp

    # ── QML bridge ────────────────────────────────────────────────────────
    ui/monitor_bridge.hpp
    ui/monitor_bridge.cpp
)

qt_add_resources(mosaic "qml_resources"
    PREFIX "/"
    BASE   "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        qml/MonitorView.qml
)

set_target_properties(mosaic PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER  "lab.csru.mosaic"
    MACOSX_BUNDLE_BUNDLE_VERSION  ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    # Windows GUI subsystem — suppresses the console window that otherwise
    # pops up alongside the Qt UI on launch. qt_add_executable() doesn't
    # accept WIN32_EXECUTABLE as an inline keyword the way plain
    # add_executable() does (it tries to treat it as a source file), so it
    # has to be set as a target property instead. No effect on non-Windows
    # platforms. Logging still reaches mosaic.log regardless (Logger writes
    # to a file unconditionally; only the QT_NO_DEBUG-gated qDebug() console
    # mirror is affected, and only in debug builds).
    WIN32_EXECUTABLE TRUE
)

target_include_directories(mosaic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mosaic PRIVATE
    mosaic_compiler_options
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    Qt6::Quick
    Qt6::QuickWidgets
    Qt6::Charts
)

# ── Optional hardware subsystems ───────────────────────────────────────────

if(MOSAIC_HAVE_SERIAL)
    target_sources(mosaic PRIVATE
        trigger/serial_trigger.hpp
        trigger/serial_trigger.cpp
        ui/trigger/serial_card_w.hpp
        ui/trigger/serial_card_w.cpp
    )
    target_link_libraries(mosaic PRIVATE Qt6::SerialPort)
    target_compile_definitions(mosaic PRIVATE MOSAIC_HAVE_SERIAL)
endif()

if(MOSAIC_ENABLE_CAMERAS)
    target_link_libraries(mosaic PRIVATE Pylon::Pylon)
    target_compile_definitions(mosaic PRIVATE MOSAIC_HAVE_CAMERAS)
endif()

if(MOSAIC_ENABLE_FFMPEG)
    target_link_libraries(mosaic PRIVATE
        FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
        FFmpeg::swscale FFmpeg::swresample)
    target_compile_definitions(mosaic PRIVATE MOSAIC_HAVE_FFMPEG)
endif()

if(MOSAIC_ENABLE_NVENC)
    target_compile_definitions(mosaic PRIVATE MOSAIC_HAVE_NVENC)
endif()

if(MOSAIC_ENABLE_PARALLEL_PORT)
    target_compile_definitions(mosaic PRIVATE MOSAIC_HAVE_PARALLEL_PORT)
endif()

if(MOSAIC_ENABLE_LSL)
    target_link_libraries(mosaic PRIVATE LSL::lsl)
    target_compile_definitions(mosaic PRIVATE MOSAIC_HAVE_LSL)
endif()

if(MOSAIC_ENABLE_OPENCV)
    target_link_libraries(mosaic PRIVATE
        opencv_core opencv_imgproc opencv_calib3d)
    target_compile_definitions(mosaic PRIVATE MOSAIC_HAVE_OPENCV)
endif()

# macOS: VideoToolbox via FFmpeg (requires FFMPEG to be enabled)
if(APPLE AND MOSAIC_ENABLE_FFMPEG AND NOT MOSAIC_ENABLE_NVENC)
    target_compile_definitions(mosaic PRIVATE MOSAIC_HAVE_VIDEOTOOLBOX)
endif()
