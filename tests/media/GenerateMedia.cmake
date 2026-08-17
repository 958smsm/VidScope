if(NOT FFMPEG_EXECUTABLE OR NOT EXISTS "${FFMPEG_EXECUTABLE}")
    message(FATAL_ERROR "FFMPEG_EXECUTABLE does not identify an FFmpeg CLI")
endif()
if(NOT OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR is required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(_drawtext "drawtext=text=Frame %{n}:x=8:y=8:fontsize=18:fontcolor=white:box=1:boxcolor=black@0.75")

function(generate_fixture name)
    execute_process(
        COMMAND "${FFMPEG_EXECUTABLE}" -hide_banner -loglevel error -y ${ARGN} "${OUTPUT_DIR}/${name}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        COMMAND_ECHO STDOUT
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "FFmpeg failed to generate ${name}: ${_stderr}")
    endif()
endfunction()

generate_fixture(
    cfr_no_b.mp4
    -f lavfi -i "testsrc2=size=160x90:rate=12:duration=2"
    -vf "${_drawtext}"
    -an -c:v libx264 -preset ultrafast -g 12 -bf 0 -pix_fmt yuv420p
)

generate_fixture(
    cfr_bframes.mp4
    -f lavfi -i "testsrc2=size=160x90:rate=12:duration=2"
    -vf "${_drawtext}"
    -an -c:v libx264 -preset medium -g 24 -bf 3 -pix_fmt yuv420p
)

generate_fixture(
    coarse_all_i.mp4
    -f lavfi -i "testsrc2=size=160x90:rate=10:duration=1.2"
    -vf "${_drawtext}"
    -an -c:v libx264 -preset ultrafast -g 1 -bf 0 -pix_fmt yuv420p
    -enc_time_base 1:10 -video_track_timescale 10
)

generate_fixture(
    vfr.mp4
    -f lavfi -i "testsrc2=size=160x90:rate=10:duration=2"
    -vf "settb=1/1000,setpts=N/(10*TB)-mod(N\\,2)/(20*TB),${_drawtext}"
    -fps_mode vfr -enc_time_base 1:1000
    -an -c:v libx264 -preset ultrafast -g 10 -bf 2 -pix_fmt yuv420p
)

generate_fixture(
    long_gop.mp4
    -f lavfi -i "testsrc2=size=160x90:rate=10:duration=6"
    -vf "${_drawtext}"
    -an -c:v libx264 -preset ultrafast -g 50 -keyint_min 50 -sc_threshold 0 -bf 2 -pix_fmt yuv420p
)

generate_fixture(
    nonzero_start.mkv
    -f lavfi -i "testsrc2=size=64x64:rate=5:duration=1"
    -vf "setpts=PTS+5/TB,${_drawtext}"
    -copyts -an -c:v ffv1 -level 3
)

generate_fixture(
    duplicate_pts.mkv
    -f lavfi -i "testsrc2=size=64x64:rate=1:duration=4"
    -vf "settb=1/1000,setpts=trunc(N/2)*1000"
    -fps_mode passthrough -enc_time_base 1:1000
    -an -c:v ffv1 -level 3
)

generate_fixture(
    multi_stream.mkv
    -f lavfi -i "testsrc2=size=96x54:rate=10:duration=2"
    -f lavfi -i "testsrc2=size=160x90:rate=10:duration=2"
    -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=2"
    -map 0:v:0 -map 1:v:0 -map 2:a:0
    -c:v libx264 -preset ultrafast -g 10 -bf 0 -pix_fmt yuv420p
    -c:a pcm_s16le
    -disposition:v:0 default -disposition:v:1 0
    -metadata:s:v:0 "title=Primary 96x54 video"
    -metadata:s:v:1 "title=Secondary 160x90 video"
)

generate_fixture(
    audio_only.mka
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1"
    -vn -c:a pcm_s16le
)
