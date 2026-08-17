include(FindPackageHandleStandardArgs)

set(FFMPEG_ROOT "" CACHE PATH "Root of an FFmpeg shared development SDK")

set(_FFmpeg_root_hints)
if(FFMPEG_ROOT)
    list(APPEND _FFmpeg_root_hints "${FFMPEG_ROOT}")
endif()
if(DEFINED ENV{FFMPEG_ROOT})
    list(APPEND _FFmpeg_root_hints "$ENV{FFMPEG_ROOT}")
endif()

find_path(
    FFmpeg_INCLUDE_DIR
    NAMES libavformat/avformat.h
    HINTS ${_FFmpeg_root_hints}
    PATH_SUFFIXES include
)

set(_FFmpeg_required_vars FFmpeg_INCLUDE_DIR)
foreach(_FFmpeg_component IN LISTS FFmpeg_FIND_COMPONENTS)
    find_library(
        FFmpeg_${_FFmpeg_component}_LIBRARY
        NAMES ${_FFmpeg_component} lib${_FFmpeg_component}
        HINTS ${_FFmpeg_root_hints}
        PATH_SUFFIXES lib lib64
    )
    if(FFmpeg_${_FFmpeg_component}_LIBRARY)
        set(FFmpeg_${_FFmpeg_component}_FOUND TRUE)
    else()
        set(FFmpeg_${_FFmpeg_component}_FOUND FALSE)
    endif()
    list(APPEND _FFmpeg_required_vars FFmpeg_${_FFmpeg_component}_LIBRARY)
endforeach()

find_program(
    FFmpeg_EXECUTABLE
    NAMES ffmpeg
    HINTS ${_FFmpeg_root_hints}
    PATH_SUFFIXES bin
)

find_package_handle_standard_args(
    FFmpeg
    REQUIRED_VARS ${_FFmpeg_required_vars}
    HANDLE_COMPONENTS
)

if(FFmpeg_FOUND)
    foreach(_FFmpeg_component IN LISTS FFmpeg_FIND_COMPONENTS)
        if(NOT TARGET FFmpeg::${_FFmpeg_component})
            add_library(FFmpeg::${_FFmpeg_component} INTERFACE IMPORTED)
            set_target_properties(
                FFmpeg::${_FFmpeg_component}
                PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}"
                    INTERFACE_LINK_LIBRARIES "${FFmpeg_${_FFmpeg_component}_LIBRARY}"
            )
        endif()
    endforeach()
endif()

function(vidscope_copy_ffmpeg_runtime target)
    if(NOT WIN32)
        return()
    endif()

    set(_runtime_roots)
    foreach(_root IN LISTS _FFmpeg_root_hints)
        list(APPEND _runtime_roots "${_root}/bin")
    endforeach()
    if(FFmpeg_EXECUTABLE)
        get_filename_component(_ffmpeg_bin "${FFmpeg_EXECUTABLE}" DIRECTORY)
        list(APPEND _runtime_roots "${_ffmpeg_bin}")
    endif()
    list(REMOVE_DUPLICATES _runtime_roots)

    set(_runtime_files)
    foreach(_root IN LISTS _runtime_roots)
        file(
            GLOB _root_runtime_files
            CONFIGURE_DEPENDS
            "${_root}/avcodec-*.dll"
            "${_root}/avformat-*.dll"
            "${_root}/avutil-*.dll"
            "${_root}/swscale-*.dll"
            "${_root}/swresample-*.dll"
        )
        list(APPEND _runtime_files ${_root_runtime_files})
    endforeach()

    if(NOT _runtime_files)
        message(WARNING "FFmpeg runtime DLLs were not found; VidScope may need PATH configured")
        return()
    endif()

    foreach(_runtime IN LISTS _runtime_files)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_runtime}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
        )
    endforeach()
endfunction()

mark_as_advanced(FFmpeg_INCLUDE_DIR FFmpeg_EXECUTABLE)
