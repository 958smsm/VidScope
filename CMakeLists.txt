cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

project(
    VidScope
    VERSION 0.3.0
    DESCRIPTION "Frame-accurate professional video inspection with a custom timeline"
    LANGUAGES CXX
)

option(VIDSCOPE_BUILD_TESTS "Build VidScope tests" ON)
option(VIDSCOPE_DEPLOY_RUNTIME "Copy Qt and FFmpeg runtime DLLs beside the app on Windows" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets)
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale swresample)

if(VIDSCOPE_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        find_package(Qt6 6.6 REQUIRED COMPONENTS Test)
    endif()
endif()

add_library(vidscope_engine STATIC)
target_sources(
    vidscope_engine
    PRIVATE
        app/core/Logging.cpp
        app/media/Demuxer.cpp
        app/media/FfmpegRaii.cpp
        app/media/FrameConverter.cpp
        app/media/MediaSource.cpp
        app/media/MediaTypes.cpp
        app/media/VideoDecoder.cpp
        app/playback/FrameCache.cpp
        app/playback/PlaybackController.cpp
        app/playback/PlaybackSession.cpp
        app/playback/SeekController.cpp
        app/timeline/TimelineModel.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS app
        FILES
            app/core/Cancellation.h
            app/core/Logging.h
            app/media/Demuxer.h
            app/media/FfmpegRaii.h
            app/media/FrameConverter.h
            app/media/MediaSource.h
            app/media/MediaTypes.h
            app/media/VideoDecoder.h
            app/playback/FrameCache.h
            app/playback/FrameQueue.h
            app/playback/PlaybackController.h
            app/playback/PlaybackSession.h
            app/playback/SeekController.h
            app/timeline/TimelineModel.h
)

target_link_libraries(
    vidscope_engine
    PUBLIC
        Qt6::Core
        Qt6::Gui
        FFmpeg::avformat
        FFmpeg::avcodec
        FFmpeg::avutil
        FFmpeg::swscale
        FFmpeg::swresample
)
target_include_directories(vidscope_engine PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/app>")
target_compile_definitions(vidscope_engine PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)

add_library(vidscope_ui STATIC)
target_sources(
    vidscope_ui
    PRIVATE
        app/render/VideoViewport.cpp
        app/timeline/TimelineWidget.cpp
        app/widgets/MainWindow.cpp
        app/widgets/ShortcutEditorDialog.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS app
        FILES
            app/render/VideoViewport.h
            app/timeline/TimelineWidget.h
            app/widgets/MainWindow.h
            app/widgets/ShortcutEditorDialog.h
)
target_link_libraries(vidscope_ui PUBLIC vidscope_engine Qt6::Widgets)
target_include_directories(vidscope_ui PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/app>")

qt_add_executable(VidScope WIN32 app/main.cpp)
target_link_libraries(VidScope PRIVATE vidscope_ui)

function(vidscope_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /EHsc)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
endfunction()

vidscope_enable_warnings(vidscope_engine)
vidscope_enable_warnings(vidscope_ui)
vidscope_enable_warnings(VidScope)

if(WIN32 AND VIDSCOPE_DEPLOY_RUNTIME)
    vidscope_copy_ffmpeg_runtime(VidScope)

    get_target_property(_vidscope_qmake Qt6::qmake IMPORTED_LOCATION)
    if(_vidscope_qmake)
        get_filename_component(_vidscope_qt_bin "${_vidscope_qmake}" DIRECTORY)
        find_program(
            VIDSCOPE_WINDEPLOYQT
            NAMES windeployqt
            HINTS "${_vidscope_qt_bin}"
            NO_DEFAULT_PATH
        )
        if(VIDSCOPE_WINDEPLOYQT)
            add_custom_command(
                TARGET VidScope
                POST_BUILD
                COMMAND "${VIDSCOPE_WINDEPLOYQT}"
                    --no-translations
                    "$<TARGET_FILE:VidScope>"
                COMMENT "Deploying Qt runtime"
                VERBATIM
            )
        endif()
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS VidScope RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" BUNDLE DESTINATION .)

if(VIDSCOPE_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()
