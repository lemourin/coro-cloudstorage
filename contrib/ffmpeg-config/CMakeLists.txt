project(ffmpeg C)

option(FFMPEG_SOURCE_DIR "ffmpeg source path" "${CMAKE_CURRENT_SOURCE_DIR}")

set(FFMPEG_DEFINITIONS
    _ISOC99_SOURCE
    _FILE_OFFSET_BITS=64
    _LARGEFILE_SOURCE
    _USE_MATH_DEFINES
    HAVE_AV_CONFIG_H
)
set(FFMPEG_PUBLIC_INCLUDES
    ${CMAKE_CURRENT_BINARY_DIR}
    ${FFMPEG_SOURCE_DIR}
    .
)
set(FFMPEG_DEPENDENCIES zlib)
if(WIN32)
    set(FFMPEG_PRIVATE_INCLUDES
        ${FFMPEG_SOURCE_DIR}/compat/atomics/win32
    )
    list(APPEND FFMPEG_DEFINITIONS
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        _WIN32_WINNT=0x0600
    )
    list(APPEND FFMPEG_DEPENDENCIES bcrypt)
    if(MSVC)
        function(patch_c_flags FLAGS)
            string(REPLACE "/GL" "" ${FLAGS} "${${FLAGS}}")
            string(REPLACE "/RTC1" "" ${FLAGS} "${${FLAGS}}")
            string(REPLACE "/Od" "/O1" ${FLAGS} "${${FLAGS}}")
            set(${FLAGS} "${${FLAGS}}" PARENT_SCOPE)
        endfunction()
        foreach(CONFIG ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
            string(TOUPPER ${CONFIG} CONFIG)
            patch_c_flags(CMAKE_C_FLAGS_${CONFIG})
        endforeach()
        patch_c_flags(CMAKE_C_FLAGS)
        set(FFMPEG_COMPILE_OPTIONS /Zc:inline-)
    endif()
endif()

include(TestBigEndian)
test_big_endian(AV_HAVE_BIGENDIAN)
configure_file(libavutil/avconfig.h.in libavutil/avconfig.h)

set(CC_IDENT "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
string(TIMESTAMP CONFIG_THIS_YEAR "%Y")

check_include_file(malloc.h HAVE_MALLOC_H)
check_include_file(windows.h HAVE_WINDOWS_H)
check_include_file(winsock2.h HAVE_WINSOCK2_H)
check_include_file(io.h HAVE_IO_H)
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(arpa/inet.h HAVE_ARPA_INET_H)
check_include_file(poll.h HAVE_POLL_H)
check_include_file(bcrypt.h HAVE_BCRYPT)

check_symbol_exists(VirtualAlloc "windows.h" HAVE_VIRTUALALLOC)
check_symbol_exists(GetSystemTimeAsFileTime "windows.h" HAVE_GETSYSTEMTIMEASFILETIME)
check_symbol_exists(MapViewOfFile "windows.h" HAVE_MAPVIEWOFFILE)
check_symbol_exists(GetProcessAffinityMask "windows.h" HAVE_GETPROCESSAFFINITYMASK)
check_symbol_exists(closesocket "winsock2.h" HAVE_CLOSESOCKET)
check_symbol_exists(Sleep "windows.h" HAVE_SLEEP)
check_symbol_exists(_aligned_malloc "malloc.h" HAVE_ALIGNED_MALLOC)
check_symbol_exists(gettimeofday "sys/time.h" HAVE_GETTIMEOFDAY)
check_symbol_exists(nanosleep "time.h" HAVE_NANOSLEEP)
check_symbol_exists(posix_memalign "stdlib.h" HAVE_POSIX_MEMALIGN)

if(WIN32)
    set(SLIBSUF ".dll")
    set(OS_NAME "win32")
    set(HAVE_W32THREADS 1)
    if(MSVC)
        set(HAVE_LIBC_MSVCRT 1)
    endif()
else()
    set(HAVE_PTHREADS 1)
    if(UNIX)
        set(OS_NAME "linux")
        set(SLIBSUF ".so")
    elseif(ANDROID)
        set(OS_NAME "android")
        set(SLIBSUF ".so")
    elseif(APPLE)
        set(OS_NAME "apple")
        set(SLIBSUF ".dylib")
    else()
        set(OS_NAME "unknown")
        set(SLIBSUF ".so")
    endif()
endif()
configure_file(config.h.in config.h)

add_library(avutil
    ${FFMPEG_SOURCE_DIR}/libavutil/adler32.c
    ${FFMPEG_SOURCE_DIR}/libavutil/aes.c
    ${FFMPEG_SOURCE_DIR}/libavutil/aes_ctr.c
    ${FFMPEG_SOURCE_DIR}/libavutil/audio_fifo.c
    ${FFMPEG_SOURCE_DIR}/libavutil/avsscanf.c
    ${FFMPEG_SOURCE_DIR}/libavutil/avstring.c
    ${FFMPEG_SOURCE_DIR}/libavutil/base64.c
    ${FFMPEG_SOURCE_DIR}/libavutil/blowfish.c
    ${FFMPEG_SOURCE_DIR}/libavutil/bprint.c
    ${FFMPEG_SOURCE_DIR}/libavutil/buffer.c
    ${FFMPEG_SOURCE_DIR}/libavutil/camellia.c
    ${FFMPEG_SOURCE_DIR}/libavutil/cast5.c
    ${FFMPEG_SOURCE_DIR}/libavutil/channel_layout.c
    ${FFMPEG_SOURCE_DIR}/libavutil/color_utils.c
    ${FFMPEG_SOURCE_DIR}/libavutil/cpu.c
    ${FFMPEG_SOURCE_DIR}/libavutil/crc.c
    ${FFMPEG_SOURCE_DIR}/libavutil/des.c
    ${FFMPEG_SOURCE_DIR}/libavutil/dict.c
    ${FFMPEG_SOURCE_DIR}/libavutil/display.c
    ${FFMPEG_SOURCE_DIR}/libavutil/dovi_meta.c
    ${FFMPEG_SOURCE_DIR}/libavutil/downmix_info.c
    ${FFMPEG_SOURCE_DIR}/libavutil/encryption_info.c
    ${FFMPEG_SOURCE_DIR}/libavutil/error.c
    ${FFMPEG_SOURCE_DIR}/libavutil/eval.c
    ${FFMPEG_SOURCE_DIR}/libavutil/fifo.c
    ${FFMPEG_SOURCE_DIR}/libavutil/file.c
    ${FFMPEG_SOURCE_DIR}/libavutil/file_open.c
    ${FFMPEG_SOURCE_DIR}/libavutil/film_grain_params.c
    ${FFMPEG_SOURCE_DIR}/libavutil/fixed_dsp.c
    ${FFMPEG_SOURCE_DIR}/libavutil/float_dsp.c
    ${FFMPEG_SOURCE_DIR}/libavutil/frame.c
    ${FFMPEG_SOURCE_DIR}/libavutil/hash.c
    ${FFMPEG_SOURCE_DIR}/libavutil/hdr_dynamic_metadata.c
    ${FFMPEG_SOURCE_DIR}/libavutil/hmac.c
    ${FFMPEG_SOURCE_DIR}/libavutil/hwcontext.c
    ${FFMPEG_SOURCE_DIR}/libavutil/imgutils.c
    ${FFMPEG_SOURCE_DIR}/libavutil/integer.c
    ${FFMPEG_SOURCE_DIR}/libavutil/intmath.c
    ${FFMPEG_SOURCE_DIR}/libavutil/lfg.c
    ${FFMPEG_SOURCE_DIR}/libavutil/lls.c
    ${FFMPEG_SOURCE_DIR}/libavutil/log.c
    ${FFMPEG_SOURCE_DIR}/libavutil/log2_tab.c
    ${FFMPEG_SOURCE_DIR}/libavutil/lzo.c
    ${FFMPEG_SOURCE_DIR}/libavutil/mastering_display_metadata.c
    ${FFMPEG_SOURCE_DIR}/libavutil/mathematics.c
    ${FFMPEG_SOURCE_DIR}/libavutil/md5.c
    ${FFMPEG_SOURCE_DIR}/libavutil/mem.c
    ${FFMPEG_SOURCE_DIR}/libavutil/murmur3.c
    ${FFMPEG_SOURCE_DIR}/libavutil/opt.c
    ${FFMPEG_SOURCE_DIR}/libavutil/parseutils.c
    ${FFMPEG_SOURCE_DIR}/libavutil/pixdesc.c
    ${FFMPEG_SOURCE_DIR}/libavutil/pixelutils.c
    ${FFMPEG_SOURCE_DIR}/libavutil/random_seed.c
    ${FFMPEG_SOURCE_DIR}/libavutil/rational.c
    ${FFMPEG_SOURCE_DIR}/libavutil/rc4.c
    ${FFMPEG_SOURCE_DIR}/libavutil/reverse.c
    ${FFMPEG_SOURCE_DIR}/libavutil/ripemd.c
    ${FFMPEG_SOURCE_DIR}/libavutil/samplefmt.c
    ${FFMPEG_SOURCE_DIR}/libavutil/sha.c
    ${FFMPEG_SOURCE_DIR}/libavutil/sha512.c
    ${FFMPEG_SOURCE_DIR}/libavutil/slicethread.c
    ${FFMPEG_SOURCE_DIR}/libavutil/spherical.c
    ${FFMPEG_SOURCE_DIR}/libavutil/stereo3d.c
    ${FFMPEG_SOURCE_DIR}/libavutil/tea.c
    ${FFMPEG_SOURCE_DIR}/libavutil/threadmessage.c
    ${FFMPEG_SOURCE_DIR}/libavutil/time.c
    ${FFMPEG_SOURCE_DIR}/libavutil/timecode.c
    ${FFMPEG_SOURCE_DIR}/libavutil/tree.c
    ${FFMPEG_SOURCE_DIR}/libavutil/twofish.c
    ${FFMPEG_SOURCE_DIR}/libavutil/tx.c
    ${FFMPEG_SOURCE_DIR}/libavutil/tx_double.c
    ${FFMPEG_SOURCE_DIR}/libavutil/tx_float.c
    ${FFMPEG_SOURCE_DIR}/libavutil/tx_int32.c
    ${FFMPEG_SOURCE_DIR}/libavutil/utils.c
    ${FFMPEG_SOURCE_DIR}/libavutil/video_enc_params.c
    ${FFMPEG_SOURCE_DIR}/libavutil/xga_font_data.c
    ${FFMPEG_SOURCE_DIR}/libavutil/xtea.c
)
target_link_libraries(avutil PRIVATE ${FFMPEG_DEPENDENCIES})
target_compile_definitions(avutil PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(avutil PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(avutil
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES}
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(swscale
    ${FFMPEG_SOURCE_DIR}/libswscale/alphablend.c
    ${FFMPEG_SOURCE_DIR}/libswscale/gamma.c
    ${FFMPEG_SOURCE_DIR}/libswscale/hscale.c
    ${FFMPEG_SOURCE_DIR}/libswscale/hscale_fast_bilinear.c
    ${FFMPEG_SOURCE_DIR}/libswscale/input.c
    ${FFMPEG_SOURCE_DIR}/libswscale/options.c
    ${FFMPEG_SOURCE_DIR}/libswscale/output.c
    ${FFMPEG_SOURCE_DIR}/libswscale/rgb2rgb.c
    ${FFMPEG_SOURCE_DIR}/libswscale/slice.c
    ${FFMPEG_SOURCE_DIR}/libswscale/swscale.c
    ${FFMPEG_SOURCE_DIR}/libswscale/swscale_unscaled.c
    ${FFMPEG_SOURCE_DIR}/libswscale/utils.c
    ${FFMPEG_SOURCE_DIR}/libswscale/vscale.c
    ${FFMPEG_SOURCE_DIR}/libswscale/yuv2rgb.c
)
target_link_libraries(swscale PRIVATE ${FFMPEG_DEPENDENCIES})
target_compile_definitions(swscale PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(swscale PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(swscale
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES}
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(swresample
    ${FFMPEG_SOURCE_DIR}/libswresample/audioconvert.c
    ${FFMPEG_SOURCE_DIR}/libswresample/dither.c
    ${FFMPEG_SOURCE_DIR}/libswresample/options.c
    ${FFMPEG_SOURCE_DIR}/libswresample/rematrix.c
    ${FFMPEG_SOURCE_DIR}/libswresample/resample.c
    ${FFMPEG_SOURCE_DIR}/libswresample/resample_dsp.c
    ${FFMPEG_SOURCE_DIR}/libswresample/swresample.c
    ${FFMPEG_SOURCE_DIR}/libswresample/swresample_frame.c
)
target_link_libraries(swresample PRIVATE ${FFMPEG_DEPENDENCIES})
target_compile_definitions(swresample PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(swresample PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(swresample
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES}
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(avcodec
    ${FFMPEG_SOURCE_DIR}/libavcodec/aac_ac3_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/aac_adtstoasc_bsf.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/aacdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/aacps_common.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/aacps_float.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/aacpsdsp_float.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/aacsbr.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/aactab.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/aandcttab.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ac3.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ac3_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ac3dec_data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ac3dec_float.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ac3dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ac3tab.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/adts_header.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/adts_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/allcodecs.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ass.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/atsc_a53.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/avcodec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/avdct.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/avfft.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/avpacket.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/avpicture.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/bitstream.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/bitstream_filter.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/bitstream_filters.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/blockdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/bmp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/bsf.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/bswapdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/cabac.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/cbrt_data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/codec_desc.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/codec_par.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/d3d11va.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/dct.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/dct32_fixed.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/dct32_float.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/decode.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/dirac.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/dv_profile.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/dvdsub.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/dvdsubdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/dynamic_hdr10_plus.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/eac3_data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/encode.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/error_resilience.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/exif.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/faandct.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/faanidct.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/faxcompr.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/fdctdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/fft_fixed_32.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/fft_float.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/fft_init_table.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/flac.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/flac_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/flacdata.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/flacdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/flacdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/fmtconvert.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/frame_thread_encoder.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/gifdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/golomb.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h263.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h263_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h263data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h263dec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h263dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h2645_parse.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_cabac.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_cavlc.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_direct.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_loopfilter.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_mb.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_parse.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_picture.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_ps.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_refs.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_sei.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264_slice.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264chroma.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264dec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264idct.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264pred.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/h264qpel.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevc_cabac.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevc_data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevc_filter.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevc_mvs.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevc_parse.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevc_ps.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevc_refs.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevc_sei.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevcdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevcdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hevcpred.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/hpeldsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/htmlsubtitles.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/idctdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/imgconvert.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/intrax8.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/intrax8dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ituh263dec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/jfdctfst.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/jfdctint.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/jni.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/jpegtables.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/jrevdct.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/kbdwin.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/lossless_videoencdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/lzw.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mathtables.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mdct15.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mdct_fixed_32.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mdct_float.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/me_cmp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mediacodec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mjpeg_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mjpegdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mjpegdec_common.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mjpegenc.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mjpegenc_common.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mjpegenc_huffman.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/motion_est.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpeg12data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpeg12framerate.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpeg4audio.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpeg4video.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpeg4video_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpeg4videodec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpeg_er.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudio.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudio_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudiodata.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudiodec_common.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudiodec_fixed.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudiodecheader.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudiodsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudiodsp_data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudiodsp_fixed.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegaudiodsp_float.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegpicture.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegutils.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegvideo.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegvideo_enc.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegvideo_motion.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegvideodata.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegvideodsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/mpegvideoencdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/msmpeg4.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/msmpeg4data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/msmpeg4dec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/msrledec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/null_bsf.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/options.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/opus.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/opus_celt.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/opus_pvq.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/opus_rc.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/opus_silk.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/opusdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/opusdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/opustab.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/parsers.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pcm-bluray.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pcm-dvd.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pcm.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pixblockdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/png.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pngdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pngdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pngenc.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/profiles.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pthread.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pthread_frame.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/pthread_slice.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/qpeldsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/qsv_api.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/ratecontrol.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/raw.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/rdft.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/rl.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/sbrdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/simple_idct.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/sinewin.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/srtdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/startcode.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/tiff.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/tiff_common.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/utils.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vc1.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vc1_block.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vc1_loopfilter.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vc1_mc.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vc1_pred.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vc1data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vc1dec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vc1dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/videodsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vorbis.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vorbis_data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vorbis_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vorbisdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vorbisdsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp3.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp3dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp56rac.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp8.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp8dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9_parser.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9_superframe_bsf.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9_superframe_split_bsf.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9block.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9dsp_10bpp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9dsp_12bpp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9dsp_8bpp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9lpf.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9mvs.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9prob.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/vp9recon.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/webp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/webvttdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/wmv2.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/wmv2data.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/wmv2dec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/wmv2dsp.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/xiph.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/xsubdec.c
    ${FFMPEG_SOURCE_DIR}/libavcodec/xvididct.c
)
target_link_libraries(avcodec PRIVATE ${FFMPEG_DEPENDENCIES} swscale swresample)
target_compile_definitions(avcodec PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(avcodec PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(avcodec
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES}
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(avformat
    ${FFMPEG_SOURCE_DIR}/libavformat/ac3dec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/allformats.c
    ${FFMPEG_SOURCE_DIR}/libavformat/av1.c
    ${FFMPEG_SOURCE_DIR}/libavformat/avc.c
    ${FFMPEG_SOURCE_DIR}/libavformat/avlanguage.c
    ${FFMPEG_SOURCE_DIR}/libavformat/avidec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/avio.c
    ${FFMPEG_SOURCE_DIR}/libavformat/aviobuf.c
    ${FFMPEG_SOURCE_DIR}/libavformat/dump.c
    ${FFMPEG_SOURCE_DIR}/libavformat/flac_picture.c
    ${FFMPEG_SOURCE_DIR}/libavformat/flacdec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/flacenc.c
    ${FFMPEG_SOURCE_DIR}/libavformat/flacenc_header.c
    ${FFMPEG_SOURCE_DIR}/libavformat/format.c
    ${FFMPEG_SOURCE_DIR}/libavformat/gifdec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/http.c
    ${FFMPEG_SOURCE_DIR}/libavformat/httpauth.c
    ${FFMPEG_SOURCE_DIR}/libavformat/hevc.c
    ${FFMPEG_SOURCE_DIR}/libavformat/id3v1.c
    ${FFMPEG_SOURCE_DIR}/libavformat/id3v2.c
    ${FFMPEG_SOURCE_DIR}/libavformat/img2.c
    ${FFMPEG_SOURCE_DIR}/libavformat/img2dec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/isom.c
    ${FFMPEG_SOURCE_DIR}/libavformat/isom_tags.c
    ${FFMPEG_SOURCE_DIR}/libavformat/matroska.c
    ${FFMPEG_SOURCE_DIR}/libavformat/matroskadec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/matroskaenc.c
    ${FFMPEG_SOURCE_DIR}/libavformat/metadata.c
    ${FFMPEG_SOURCE_DIR}/libavformat/mov.c
    ${FFMPEG_SOURCE_DIR}/libavformat/movenc.c
    ${FFMPEG_SOURCE_DIR}/libavformat/movenccenc.c
    ${FFMPEG_SOURCE_DIR}/libavformat/movenchint.c
    ${FFMPEG_SOURCE_DIR}/libavformat/mov_chan.c
    ${FFMPEG_SOURCE_DIR}/libavformat/mov_esds.c
    ${FFMPEG_SOURCE_DIR}/libavformat/mp3dec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/mux.c
    ${FFMPEG_SOURCE_DIR}/libavformat/network.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggdec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparsecelt.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparsedirac.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparseflac.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparseogm.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparseopus.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparseskeleton.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparsespeex.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparsetheora.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparsevorbis.c
    ${FFMPEG_SOURCE_DIR}/libavformat/oggparsevp8.c
    ${FFMPEG_SOURCE_DIR}/libavformat/options.c
    ${FFMPEG_SOURCE_DIR}/libavformat/os_support.c
    ${FFMPEG_SOURCE_DIR}/libavformat/pcm.c
    ${FFMPEG_SOURCE_DIR}/libavformat/pcmdec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/protocols.c
    ${FFMPEG_SOURCE_DIR}/libavformat/qtpalette.c
    ${FFMPEG_SOURCE_DIR}/libavformat/rawdec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/rawutils.c
    ${FFMPEG_SOURCE_DIR}/libavformat/replaygain.c
    ${FFMPEG_SOURCE_DIR}/libavformat/riff.c
    ${FFMPEG_SOURCE_DIR}/libavformat/riffdec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/riffenc.c
    ${FFMPEG_SOURCE_DIR}/libavformat/rmsipr.c
    ${FFMPEG_SOURCE_DIR}/libavformat/rtp.c
    ${FFMPEG_SOURCE_DIR}/libavformat/rtpenc_chain.c
    ${FFMPEG_SOURCE_DIR}/libavformat/sdp.c
    ${FFMPEG_SOURCE_DIR}/libavformat/tcp.c
    ${FFMPEG_SOURCE_DIR}/libavformat/url.c
    ${FFMPEG_SOURCE_DIR}/libavformat/urldecode.c
    ${FFMPEG_SOURCE_DIR}/libavformat/utils.c
    ${FFMPEG_SOURCE_DIR}/libavformat/vorbiscomment.c
    ${FFMPEG_SOURCE_DIR}/libavformat/vpcc.c
    ${FFMPEG_SOURCE_DIR}/libavformat/wavdec.c
    ${FFMPEG_SOURCE_DIR}/libavformat/wv.c
)
target_link_libraries(avformat PRIVATE ${FFMPEG_DEPENDENCIES} swscale avutil avcodec)
target_compile_definitions(avformat PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(avformat PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(avformat
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES}
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(avfilter
    ${FFMPEG_SOURCE_DIR}/libavfilter/allfilters.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/audio.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/avfilter.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/avfiltergraph.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/buffersink.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/buffersrc.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/drawutils.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/fifo.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/formats.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/framepool.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/framequeue.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/graphdump.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/graphparser.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/pthread.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/scale_eval.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/transform.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/vf_scale.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/vf_thumbnail.c
    ${FFMPEG_SOURCE_DIR}/libavfilter/video.c
)
target_link_libraries(avfilter PRIVATE ${FFMPEG_DEPENDENCIES})
target_compile_definitions(avfilter PRIVATE
    ${FFMPEG_DEFINITIONS}
    M_PI_4=0.785398163397448309616
    M_2_PI=0.636619772367581343076
)
target_compile_options(avfilter PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(avfilter
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES}
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)
