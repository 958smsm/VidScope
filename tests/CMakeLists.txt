add_executable(
    vidscope_unit_tests
    TestHarness.h
    unit/TestMain.cpp
    unit/FrameCacheTests.cpp
    unit/FrameQueueTests.cpp
    unit/SeekControllerTests.cpp
    unit/TimestampTests.cpp
    unit/VisibleImageTests.cpp
)
target_include_directories(vidscope_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vidscope_unit_tests PRIVATE vidscope_engine)
vidscope_enable_warnings(vidscope_unit_tests)
add_test(NAME vidscope.unit COMMAND vidscope_unit_tests)

add_executable(
    vidscope_integration_tests
    TestHarness.h
    integration/DecoderIntegrationTests.cpp
)
target_include_directories(vidscope_integration_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vidscope_integration_tests PRIVATE vidscope_engine)
vidscope_enable_warnings(vidscope_integration_tests)

add_executable(
    vidscope_phase2_stress_tests
    TestHarness.h
    integration/Phase2StressTests.cpp
)
target_include_directories(vidscope_phase2_stress_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vidscope_phase2_stress_tests PRIVATE vidscope_engine)
vidscope_enable_warnings(vidscope_phase2_stress_tests)

function(vidscope_configure_test_runtime test_name)
    if(NOT WIN32)
        return()
    endif()

    set(_modifications)
    get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
    if(_qmake)
        get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
        list(APPEND _modifications "PATH=path_list_prepend:${_qt_bin}")
    endif()
    if(FFmpeg_EXECUTABLE)
        get_filename_component(_ffmpeg_bin "${FFmpeg_EXECUTABLE}" DIRECTORY)
        list(APPEND _modifications "PATH=path_list_prepend:${_ffmpeg_bin}")
    endif()
    if(_modifications)
        set_property(TEST ${test_name} PROPERTY ENVIRONMENT_MODIFICATION "${_modifications}")
    endif()
endfunction()

function(vidscope_configure_offscreen_test test_name)
    set(_environment "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
        if(_qmake)
            get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
            get_filename_component(_qt_prefix "${_qt_bin}" DIRECTORY)
            list(APPEND _environment "QT_PLUGIN_PATH=${_qt_prefix}/plugins")
        endif()
    endif()
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${_environment}")
endfunction()

vidscope_configure_test_runtime(vidscope.unit)

if(FFmpeg_EXECUTABLE)
    set(_vidscope_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/media")
    add_test(
        NAME vidscope.generate_media
        COMMAND
            "${CMAKE_COMMAND}"
            "-DFFMPEG_EXECUTABLE=${FFmpeg_EXECUTABLE}"
            "-DOUTPUT_DIR=${_vidscope_fixture_dir}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/media/GenerateMedia.cmake"
    )
    set_tests_properties(vidscope.generate_media PROPERTIES FIXTURES_SETUP VidScopeMedia TIMEOUT 120)

    add_test(
        NAME vidscope.integration
        COMMAND vidscope_integration_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.integration
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            TIMEOUT 120
    )
    vidscope_configure_test_runtime(vidscope.integration)

    add_test(
        NAME vidscope.phase2_stress
        COMMAND vidscope_phase2_stress_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.phase2_stress
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            TIMEOUT 120
    )
    vidscope_configure_test_runtime(vidscope.phase2_stress)

    add_test(
        NAME vidscope.ui_media_smoke
        COMMAND VidScope --media-smoke-test "${_vidscope_fixture_dir}/cfr_bframes.mp4"
    )
    set_tests_properties(
        vidscope.ui_media_smoke
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            TIMEOUT 45
    )
    vidscope_configure_offscreen_test(vidscope.ui_media_smoke)
    vidscope_configure_test_runtime(vidscope.ui_media_smoke)
else()
    message(WARNING "FFmpeg CLI was not found; synthetic media integration tests are disabled")
endif()

add_test(NAME vidscope.ui_smoke COMMAND VidScope --smoke-test)
set_tests_properties(vidscope.ui_smoke PROPERTIES TIMEOUT 30)
vidscope_configure_offscreen_test(vidscope.ui_smoke)
vidscope_configure_test_runtime(vidscope.ui_smoke)
