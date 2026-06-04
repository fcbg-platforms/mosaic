qt_add_executable(mosaic
    main.cpp
    core/application.cpp       core/application.hpp
    core/settings.cpp          core/settings.hpp
    video/video_manager.cpp    video/video_manager.hpp
    video/camera_grabber.cpp   video/camera_grabber.hpp
    video/video_encoder.cpp    video/video_encoder.hpp
    audio/audio_manager.cpp    audio/audio_manager.hpp
    audio/audio_recorder.cpp   audio/audio_recorder.hpp
    trigger/trigger_manager.cpp trigger/trigger_manager.hpp
    trigger/trigger.cpp         trigger/trigger.hpp
    ui/main_window.cpp         ui/main_window.hpp
    utils/logger.cpp           utils/logger.hpp
    utils/ring_buffer.hpp
    utils/timestamp.hpp
)

target_include_directories(mosaic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mosaic PRIVATE
    mosaic_compiler_options
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort Qt6::Multimedia
    Pylon::Pylon
    FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
    opencv_core opencv_imgproc
)
