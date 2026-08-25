add_executable(
    vidscope_unit_tests
    TestHarness.h
    unit/AnalysisTests.cpp
    unit/TestMain.cpp
    unit/FilmstripModelTests.cpp
    unit/FrameCacheTests.cpp
    unit/FrameQueueTests.cpp
    unit/SeekControllerTests.cpp
    unit/ThumbnailCacheTests.cpp
    unit/ThumbnailSchedulerTests.cpp
    unit/TimelineModelTests.cpp
    unit/TimestampTests.cpp
    unit/VisibleImageTests.cpp
)
target_include_directories(vidscope_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vidscope_unit_tests PRIVATE vidscope_engine)
vidscope_enable_warnings(vidscope_unit_tests)
add_test(NAME vidscope.unit COMMAND vidscope_unit_tests)
set_tests_properties(
    vidscope.unit
    PROPERTIES
        LABELS "phase1;phase2;phase3;phase4;phase5;phase6;unit"
        TIMEOUT 120
)

add_executable(
    vidscope_integration_tests
    TestHarness.h
    integration/DecoderIntegrationTests.cpp
)
target_include_directories(vidscope_integration_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vidscope_integration_tests PRIVATE vidscope_engine)
vidscope_enable_warnings(vidscope_integration_tests)

add_executable(
    vidscope_phase1_foundation_tests
    TestHarness.h
    integration/Phase1FoundationTests.cpp
)
target_include_directories(
    vidscope_phase1_foundation_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(vidscope_phase1_foundation_tests PRIVATE vidscope_ui)
vidscope_enable_warnings(vidscope_phase1_foundation_tests)

add_executable(
    vidscope_phase2_stress_tests
    TestHarness.h
    integration/Phase2StressTests.cpp
)
target_include_directories(vidscope_phase2_stress_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vidscope_phase2_stress_tests PRIVATE vidscope_engine)
vidscope_enable_warnings(vidscope_phase2_stress_tests)

add_executable(
    vidscope_timeline_widget_tests
    TestHarness.h
    integration/TimelineWidgetTests.cpp
)
target_include_directories(vidscope_timeline_widget_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vidscope_timeline_widget_tests PRIVATE vidscope_ui Qt6::Test)
vidscope_enable_warnings(vidscope_timeline_widget_tests)

add_executable(
    vidscope_timeline_widget_precision_tests
    TestHarness.h
    integration/TimelineWidgetPrecisionTests.cpp
)
target_include_directories(
    vidscope_timeline_widget_precision_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(
    vidscope_timeline_widget_precision_tests
    PRIVATE
        vidscope_ui
        Qt6::Test
)
vidscope_enable_warnings(vidscope_timeline_widget_precision_tests)

add_executable(
    vidscope_timeline_media_tests
    TestHarness.h
    integration/TimelineMediaIntegrationTests.cpp
)
target_include_directories(vidscope_timeline_media_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vidscope_timeline_media_tests PRIVATE vidscope_engine)
vidscope_enable_warnings(vidscope_timeline_media_tests)

add_executable(
    vidscope_phase3_timeline_stress_tests
    TestHarness.h
    integration/Phase3TimelineStressTests.cpp
)
target_include_directories(
    vidscope_phase3_timeline_stress_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(
    vidscope_phase3_timeline_stress_tests
    PRIVATE
        vidscope_ui
        Qt6::Test
)
vidscope_enable_warnings(vidscope_phase3_timeline_stress_tests)

add_executable(
    vidscope_timeline_end_to_end_tests
    TestHarness.h
    integration/TimelineEndToEndTests.cpp
)
target_include_directories(
    vidscope_timeline_end_to_end_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(
    vidscope_timeline_end_to_end_tests
    PRIVATE
        vidscope_ui
        Qt6::Test
)
vidscope_enable_warnings(vidscope_timeline_end_to_end_tests)

add_executable(
    vidscope_phase4_hover_preview_tests
    TestHarness.h
    integration/Phase4HoverPreviewTests.cpp
)
target_include_directories(
    vidscope_phase4_hover_preview_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(
    vidscope_phase4_hover_preview_tests
    PRIVATE
        vidscope_ui
        Qt6::Test
)
vidscope_enable_warnings(vidscope_phase4_hover_preview_tests)

add_executable(
    vidscope_phase5_filmstrip_tests
    TestHarness.h
    integration/Phase5FilmstripTests.cpp
)
target_include_directories(
    vidscope_phase5_filmstrip_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(
    vidscope_phase5_filmstrip_tests
    PRIVATE
        vidscope_ui
        Qt6::Test
)
vidscope_enable_warnings(vidscope_phase5_filmstrip_tests)

add_executable(
    vidscope_phase6_analysis_tests
    TestHarness.h
    integration/Phase6AnalysisTests.cpp
)
target_include_directories(
    vidscope_phase6_analysis_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(
    vidscope_phase6_analysis_tests
    PRIVATE
        vidscope_engine
        Qt6::Test
)
vidscope_enable_warnings(vidscope_phase6_analysis_tests)

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

add_test(NAME vidscope.timeline_widget COMMAND vidscope_timeline_widget_tests)
set_tests_properties(
    vidscope.timeline_widget
    PROPERTIES
        LABELS "phase3;ui"
        TIMEOUT 30
)
vidscope_configure_offscreen_test(vidscope.timeline_widget)
vidscope_configure_test_runtime(vidscope.timeline_widget)

add_test(
    NAME vidscope.timeline_widget_precision
    COMMAND vidscope_timeline_widget_precision_tests
)
set_tests_properties(
    vidscope.timeline_widget_precision
    PROPERTIES
        LABELS "phase3;ui"
        TIMEOUT 30
)
vidscope_configure_offscreen_test(vidscope.timeline_widget_precision)
vidscope_configure_test_runtime(vidscope.timeline_widget_precision)

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
    set_tests_properties(
        vidscope.generate_media
        PROPERTIES
            FIXTURES_SETUP VidScopeMedia
            LABELS "media-fixture"
            TIMEOUT 120
    )

    add_test(
        NAME vidscope.phase1_foundation
        COMMAND vidscope_phase1_foundation_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.phase1_foundation
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS "phase1;media;ui"
            RESOURCE_LOCK VidScopeGuiMedia
            TIMEOUT 120
    )
    vidscope_configure_offscreen_test(vidscope.phase1_foundation)
    vidscope_configure_test_runtime(vidscope.phase1_foundation)

    add_test(
        NAME vidscope.integration
        COMMAND vidscope_integration_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.integration
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS "phase2;media"
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
            LABELS "phase2;stress"
            TIMEOUT 120
    )
    vidscope_configure_test_runtime(vidscope.phase2_stress)

    add_test(
        NAME vidscope.timeline_media
        COMMAND vidscope_timeline_media_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.timeline_media
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS "phase3;media"
            TIMEOUT 120
    )
    vidscope_configure_test_runtime(vidscope.timeline_media)

    add_test(
        NAME vidscope.phase3_timeline_stress
        COMMAND vidscope_phase3_timeline_stress_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.phase3_timeline_stress
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS "phase3;stress;ui"
            RESOURCE_LOCK VidScopeGuiMedia
            TIMEOUT 120
    )
    vidscope_configure_offscreen_test(vidscope.phase3_timeline_stress)
    vidscope_configure_test_runtime(vidscope.phase3_timeline_stress)

    add_test(
        NAME vidscope.timeline_end_to_end
        COMMAND vidscope_timeline_end_to_end_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.timeline_end_to_end
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS "phase3;media;ui"
            RESOURCE_LOCK VidScopeGuiMedia
            TIMEOUT 120
    )
    vidscope_configure_offscreen_test(vidscope.timeline_end_to_end)
    vidscope_configure_test_runtime(vidscope.timeline_end_to_end)

    add_test(
        NAME vidscope.phase4_hover_preview
        COMMAND vidscope_phase4_hover_preview_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.phase4_hover_preview
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS "phase4;media;ui;thumbnail"
            RESOURCE_LOCK VidScopeGuiMedia
            TIMEOUT 120
    )
    vidscope_configure_offscreen_test(vidscope.phase4_hover_preview)
    vidscope_configure_test_runtime(vidscope.phase4_hover_preview)

    add_test(
        NAME vidscope.phase5_filmstrip
        COMMAND vidscope_phase5_filmstrip_tests "${_vidscope_fixture_dir}"
    )
    set_tests_properties(
        vidscope.phase5_filmstrip
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS "phase5;media;ui;thumbnail;filmstrip"
            RESOURCE_LOCK VidScopeGuiMedia
            TIMEOUT 180
    )
    vidscope_configure_offscreen_test(vidscope.phase5_filmstrip)
    vidscope_configure_test_runtime(vidscope.phase5_filmstrip)

    add_test(
        NAME vidscope.phase6_analysis
        COMMAND vidscope_phase6_analysis_tests ${_vidscope_fixture_dir}
    )
    set_tests_properties(
        vidscope.phase6_analysis
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS phase6
            TIMEOUT 180
    )
    vidscope_configure_offscreen_test(vidscope.phase6_analysis)
    vidscope_configure_test_runtime(vidscope.phase6_analysis)

    add_test(
        NAME vidscope.ui_media_smoke
        COMMAND VidScope --media-smoke-test "${_vidscope_fixture_dir}/cfr_bframes.mp4"
    )
    set_tests_properties(
        vidscope.ui_media_smoke
        PROPERTIES
            FIXTURES_REQUIRED VidScopeMedia
            LABELS "phase1;phase2;phase3;phase4;phase5;phase6;ui"
            RESOURCE_LOCK VidScopeGuiMedia
            TIMEOUT 45
    )
    vidscope_configure_offscreen_test(vidscope.ui_media_smoke)
    vidscope_configure_test_runtime(vidscope.ui_media_smoke)
else()
    message(WARNING "FFmpeg CLI was not found; synthetic media integration tests are disabled")
endif()

add_test(NAME vidscope.ui_smoke COMMAND VidScope --smoke-test)
set_tests_properties(
    vidscope.ui_smoke
    PROPERTIES
        LABELS "phase1;phase3;phase4;phase5;phase6;ui"
        TIMEOUT 30
)
vidscope_configure_offscreen_test(vidscope.ui_smoke)
vidscope_configure_test_runtime(vidscope.ui_smoke)
