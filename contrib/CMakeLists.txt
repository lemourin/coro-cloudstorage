add_subdirectory(coro-http EXCLUDE_FROM_ALL)
add_subdirectory(json EXCLUDE_FROM_ALL)
set(BUILD_TESTING 0 CACHE BOOL "" FORCE)
if(MSVC AND ("${CMAKE_SIZEOF_VOID_P}" STREQUAL "4" OR
             CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(DISABLE_ASM ON CACHE BOOL "")
endif()
if(MINGW OR (MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    set(DISABLE_SSSE3 ON CACHE BOOL "")
endif()
add_subdirectory(cryptopp EXCLUDE_FROM_ALL)
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(cryptopp-static PUBLIC __EXCEPTIONS CRYPTOPP_SUPPORT_CLANG_CL)
endif()
add_library(cryptopp ALIAS cryptopp-static)
if(ANDROID)
    target_include_directories(cryptopp-object PRIVATE ${CMAKE_ANDROID_NDK}/sources/android/cpufeatures)
endif()

set(USE_CRYPTOPP 1 CACHE BOOL "" FORCE)
set(USE_OPENSSL 0 CACHE BOOL "" FORCE)
set(USE_THIRDPARTY_FROM_VCPKG 0 CACHE BOOL "" FORCE)
set(USE_ASIO 0 CACHE BOOL "" FORCE)
set(USE_CURL 0 CACHE BOOL "" FORCE)
set(USE_SQLITE 0 CACHE BOOL "" FORCE)
set(USE_MEDIAINFO 0 CACHE BOOL "" FORCE)
set(USE_FREEIMAGE 0 CACHE BOOL "" FORCE)
set(USE_SODIUM 0 CACHE BOOL "" FORCE)
set(ENABLE_SYNC 0 CACHE BOOL "" FORCE)
set(ENABLE_CHAT 0 CACHE BOOL "" FORCE)
set(ENABLE_LOG_PERFORMANCE 0 CACHE BOOL "" FORCE)
set(HAVE_FFMPEG 0 CACHE BOOL "" FORCE)
set(USE_WEBRTC 0 CACHE BOOL "" FORCE)
set(USE_LIBUV 0 CACHE BOOL "" FORCE)
set(USE_QT 0 CACHE BOOL "" FORCE)
set(USE_PDFIUM 0 CACHE BOOL "" FORCE)
set(USE_LIBRAW 0 CACHE BOOL "" FORCE)
add_subdirectory(megasdk/contrib/cmake EXCLUDE_FROM_ALL)

target_include_directories(Mega PUBLIC
    .
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/include
)

if(ANDROID)
    target_compile_definitions(Mega PRIVATE _LIBCPP_HAS_NO_OFF_T_FUNCTIONS)
endif()

add_subdirectory(pugixml EXCLUDE_FROM_ALL)
set(ZLIB_COMPAT 1 CACHE BOOL "")
add_subdirectory(zlib-ng EXCLUDE_FROM_ALL)

add_subdirectory(fmt EXCLUDE_FROM_ALL)

set(FFMPEG_DEFINITIONS
    _ISOC99_SOURCE
    _FILE_OFFSET_BITS=64
    _LARGEFILE_SOURCE
    _USE_MATH_DEFINES
    HAVE_AV_CONFIG_H
)
set(FFMPEG_PUBLIC_INCLUDES
    ${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-config
    ffmpeg-config
    ffmpeg
)
set(FFMPEG_DEPENDENCIES zlib)
if(WIN32)
    set(FFMPEG_PRIVATE_INCLUDES
        ffmpeg/compat/atomics/win32
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
configure_file(ffmpeg-config/libavutil/avconfig.h.in ffmpeg-config/libavutil/avconfig.h)

set(CC_IDENT "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
string(TIMESTAMP CONFIG_THIS_YEAR "%Y")

if(WIN32)
    set(SLIBSUF ".dll")
    set(OS_NAME "win32")
    set(HAVE_WINDOWS_H 1)
    set(HAVE_WINSOCK2_H 1)
    set(HAVE_W32THREADS 1)
    set(HAVE_VIRTUALALLOC 1)
    set(HAVE_BCRYPT 1)
    set(HAVE_IO_H 1)
    set(HAVE_GETSYSTEMTIMEASFILETIME 1)
    set(HAVE_MAPVIEWOFFILE 1)
    set(HAVE_GETPROCESSAFFINITYMASK 1)
    set(HAVE_CLOSESOCKET 1)
    set(HAVE_SLEEP 1)
    set(HAVE_ALIGNED_MALLOC 1)
    if(MSVC)
        set(HAVE_LIBC_MSVCRT 1)
    endif()
else()
    set(SLIBSUF ".so")
    set(HAVE_UNISTD_H 1)
    set(HAVE_PTHREADS 1)
    set(HAVE_GETTIMEOFDAY 1)
    set(HAVE_ARPA_INET_H 1)
    set(HAVE_POLL_H 1)
    set(HAVE_NANOSLEEP 1)
    set(HAVE_POSIX_MEMALIGN 1)
    if(UNIX)
        set(OS_NAME "linux")
    elseif(ANDROID)
        set(OS_NAME "android")
    elseif(APPLE)
        set(OS_NAME "apple")
    else()
        set(OS_NAME "unknown")
    endif()
endif()
configure_file(ffmpeg-config/config.h.in ffmpeg-config/config.h)

add_library(avutil
    ffmpeg/libavutil/adler32.c
    ffmpeg/libavutil/aes.c
    ffmpeg/libavutil/aes_ctr.c
    ffmpeg/libavutil/audio_fifo.c
    ffmpeg/libavutil/avsscanf.c
    ffmpeg/libavutil/avstring.c
    ffmpeg/libavutil/base64.c
    ffmpeg/libavutil/blowfish.c
    ffmpeg/libavutil/bprint.c
    ffmpeg/libavutil/buffer.c
    ffmpeg/libavutil/camellia.c
    ffmpeg/libavutil/cast5.c
    ffmpeg/libavutil/channel_layout.c
    ffmpeg/libavutil/color_utils.c
    ffmpeg/libavutil/cpu.c
    ffmpeg/libavutil/crc.c
    ffmpeg/libavutil/des.c
    ffmpeg/libavutil/dict.c
    ffmpeg/libavutil/display.c
    ffmpeg/libavutil/dovi_meta.c
    ffmpeg/libavutil/downmix_info.c
    ffmpeg/libavutil/encryption_info.c
    ffmpeg/libavutil/error.c
    ffmpeg/libavutil/eval.c
    ffmpeg/libavutil/fifo.c
    ffmpeg/libavutil/file.c
    ffmpeg/libavutil/file_open.c
    ffmpeg/libavutil/film_grain_params.c
    ffmpeg/libavutil/fixed_dsp.c
    ffmpeg/libavutil/float_dsp.c
    ffmpeg/libavutil/frame.c
    ffmpeg/libavutil/hash.c
    ffmpeg/libavutil/hdr_dynamic_metadata.c
    ffmpeg/libavutil/hmac.c
    ffmpeg/libavutil/hwcontext.c
    ffmpeg/libavutil/imgutils.c
    ffmpeg/libavutil/integer.c
    ffmpeg/libavutil/intmath.c
    ffmpeg/libavutil/lfg.c
    ffmpeg/libavutil/lls.c
    ffmpeg/libavutil/log.c
    ffmpeg/libavutil/log2_tab.c
    ffmpeg/libavutil/lzo.c
    ffmpeg/libavutil/mastering_display_metadata.c
    ffmpeg/libavutil/mathematics.c
    ffmpeg/libavutil/md5.c
    ffmpeg/libavutil/mem.c
    ffmpeg/libavutil/murmur3.c
    ffmpeg/libavutil/opt.c
    ffmpeg/libavutil/parseutils.c
    ffmpeg/libavutil/pixdesc.c
    ffmpeg/libavutil/pixelutils.c
    ffmpeg/libavutil/random_seed.c
    ffmpeg/libavutil/rational.c
    ffmpeg/libavutil/rc4.c
    ffmpeg/libavutil/reverse.c
    ffmpeg/libavutil/ripemd.c
    ffmpeg/libavutil/samplefmt.c
    ffmpeg/libavutil/sha.c
    ffmpeg/libavutil/sha512.c
    ffmpeg/libavutil/slicethread.c
    ffmpeg/libavutil/spherical.c
    ffmpeg/libavutil/stereo3d.c
    ffmpeg/libavutil/tea.c
    ffmpeg/libavutil/threadmessage.c
    ffmpeg/libavutil/time.c
    ffmpeg/libavutil/timecode.c
    ffmpeg/libavutil/tree.c
    ffmpeg/libavutil/twofish.c
    ffmpeg/libavutil/tx.c
    ffmpeg/libavutil/tx_double.c
    ffmpeg/libavutil/tx_float.c
    ffmpeg/libavutil/tx_int32.c
    ffmpeg/libavutil/utils.c
    ffmpeg/libavutil/video_enc_params.c
    ffmpeg/libavutil/xga_font_data.c
    ffmpeg/libavutil/xtea.c
)
target_link_libraries(avutil PRIVATE ${FFMPEG_DEPENDENCIES})
target_compile_definitions(avutil PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(avutil PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(avutil 
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES} 
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(swscale
    ffmpeg/libswscale/alphablend.c
    ffmpeg/libswscale/gamma.c
    ffmpeg/libswscale/hscale.c
    ffmpeg/libswscale/hscale_fast_bilinear.c
    ffmpeg/libswscale/input.c
    ffmpeg/libswscale/options.c
    ffmpeg/libswscale/output.c
    ffmpeg/libswscale/rgb2rgb.c
    ffmpeg/libswscale/slice.c
    ffmpeg/libswscale/swscale.c
    ffmpeg/libswscale/swscale_unscaled.c
    ffmpeg/libswscale/utils.c
    ffmpeg/libswscale/vscale.c
    ffmpeg/libswscale/yuv2rgb.c
)
target_link_libraries(swscale PRIVATE ${FFMPEG_DEPENDENCIES})
target_compile_definitions(swscale PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(swscale PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(swscale
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES}
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(swresample
    ffmpeg/libswresample/audioconvert.c
    ffmpeg/libswresample/dither.c
    ffmpeg/libswresample/options.c
    ffmpeg/libswresample/rematrix.c
    ffmpeg/libswresample/resample.c
    ffmpeg/libswresample/resample_dsp.c
    ffmpeg/libswresample/swresample.c
    ffmpeg/libswresample/swresample_frame.c
)
target_link_libraries(swresample PRIVATE ${FFMPEG_DEPENDENCIES})
target_compile_definitions(swresample PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(swresample PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(swresample
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES}
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(avcodec
    ffmpeg/libavcodec/aac_ac3_parser.c
    ffmpeg/libavcodec/aac_adtstoasc_bsf.c
    ffmpeg/libavcodec/aacdec.c
    ffmpeg/libavcodec/aacps_common.c
    ffmpeg/libavcodec/aacps_float.c
    ffmpeg/libavcodec/aacpsdsp_float.c
    ffmpeg/libavcodec/aacsbr.c
    ffmpeg/libavcodec/aactab.c
    ffmpeg/libavcodec/aandcttab.c
    ffmpeg/libavcodec/ac3.c
    ffmpeg/libavcodec/ac3_parser.c
    ffmpeg/libavcodec/ac3dec_data.c
    ffmpeg/libavcodec/ac3dec_float.c
    ffmpeg/libavcodec/ac3dsp.c
    ffmpeg/libavcodec/ac3tab.c
    ffmpeg/libavcodec/adts_header.c
    ffmpeg/libavcodec/adts_parser.c
    ffmpeg/libavcodec/allcodecs.c
    ffmpeg/libavcodec/ass.c
    ffmpeg/libavcodec/atsc_a53.c
    ffmpeg/libavcodec/avcodec.c
    ffmpeg/libavcodec/avdct.c
    ffmpeg/libavcodec/avfft.c
    ffmpeg/libavcodec/avpacket.c
    ffmpeg/libavcodec/avpicture.c
    ffmpeg/libavcodec/bitstream.c
    ffmpeg/libavcodec/bitstream_filter.c
    ffmpeg/libavcodec/bitstream_filters.c
    ffmpeg/libavcodec/blockdsp.c
    ffmpeg/libavcodec/bmp.c
    ffmpeg/libavcodec/bsf.c
    ffmpeg/libavcodec/bswapdsp.c
    ffmpeg/libavcodec/cabac.c
    ffmpeg/libavcodec/cbrt_data.c
    ffmpeg/libavcodec/codec_desc.c
    ffmpeg/libavcodec/codec_par.c
    ffmpeg/libavcodec/d3d11va.c
    ffmpeg/libavcodec/dct.c
    ffmpeg/libavcodec/dct32_fixed.c
    ffmpeg/libavcodec/dct32_float.c
    ffmpeg/libavcodec/decode.c
    ffmpeg/libavcodec/dirac.c
    ffmpeg/libavcodec/dv_profile.c
    ffmpeg/libavcodec/dvdsub.c
    ffmpeg/libavcodec/dvdsubdec.c
    ffmpeg/libavcodec/dynamic_hdr10_plus.c
    ffmpeg/libavcodec/eac3_data.c
    ffmpeg/libavcodec/encode.c
    ffmpeg/libavcodec/error_resilience.c
    ffmpeg/libavcodec/exif.c
    ffmpeg/libavcodec/faandct.c
    ffmpeg/libavcodec/faanidct.c
    ffmpeg/libavcodec/faxcompr.c
    ffmpeg/libavcodec/fdctdsp.c
    ffmpeg/libavcodec/fft_fixed_32.c
    ffmpeg/libavcodec/fft_float.c
    ffmpeg/libavcodec/fft_init_table.c
    ffmpeg/libavcodec/flac.c
    ffmpeg/libavcodec/flac_parser.c
    ffmpeg/libavcodec/flacdata.c
    ffmpeg/libavcodec/flacdec.c
    ffmpeg/libavcodec/flacdsp.c
    ffmpeg/libavcodec/fmtconvert.c
    ffmpeg/libavcodec/frame_thread_encoder.c
    ffmpeg/libavcodec/gifdec.c
    ffmpeg/libavcodec/golomb.c
    ffmpeg/libavcodec/h263.c
    ffmpeg/libavcodec/h263_parser.c
    ffmpeg/libavcodec/h263data.c
    ffmpeg/libavcodec/h263dec.c
    ffmpeg/libavcodec/h263dsp.c
    ffmpeg/libavcodec/h2645_parse.c
    ffmpeg/libavcodec/h264_cabac.c
    ffmpeg/libavcodec/h264_cavlc.c
    ffmpeg/libavcodec/h264_direct.c
    ffmpeg/libavcodec/h264_loopfilter.c
    ffmpeg/libavcodec/h264_mb.c
    ffmpeg/libavcodec/h264_parse.c
    ffmpeg/libavcodec/h264_picture.c
    ffmpeg/libavcodec/h264_ps.c
    ffmpeg/libavcodec/h264_refs.c
    ffmpeg/libavcodec/h264_sei.c
    ffmpeg/libavcodec/h264_slice.c
    ffmpeg/libavcodec/h264chroma.c
    ffmpeg/libavcodec/h264data.c
    ffmpeg/libavcodec/h264dec.c
    ffmpeg/libavcodec/h264dsp.c
    ffmpeg/libavcodec/h264idct.c
    ffmpeg/libavcodec/h264pred.c
    ffmpeg/libavcodec/h264qpel.c
    ffmpeg/libavcodec/hevc_cabac.c
    ffmpeg/libavcodec/hevc_data.c
    ffmpeg/libavcodec/hevc_filter.c
    ffmpeg/libavcodec/hevc_mvs.c
    ffmpeg/libavcodec/hevc_parse.c
    ffmpeg/libavcodec/hevc_ps.c
    ffmpeg/libavcodec/hevc_refs.c
    ffmpeg/libavcodec/hevc_sei.c
    ffmpeg/libavcodec/hevcdec.c
    ffmpeg/libavcodec/hevcdsp.c
    ffmpeg/libavcodec/hevcpred.c
    ffmpeg/libavcodec/hpeldsp.c
    ffmpeg/libavcodec/htmlsubtitles.c
    ffmpeg/libavcodec/idctdsp.c
    ffmpeg/libavcodec/imgconvert.c
    ffmpeg/libavcodec/intrax8.c
    ffmpeg/libavcodec/intrax8dsp.c
    ffmpeg/libavcodec/ituh263dec.c
    ffmpeg/libavcodec/jfdctfst.c
    ffmpeg/libavcodec/jfdctint.c
    ffmpeg/libavcodec/jni.c
    ffmpeg/libavcodec/jpegtables.c
    ffmpeg/libavcodec/jrevdct.c
    ffmpeg/libavcodec/kbdwin.c
    ffmpeg/libavcodec/lossless_videoencdsp.c
    ffmpeg/libavcodec/lzw.c
    ffmpeg/libavcodec/mathtables.c
    ffmpeg/libavcodec/mdct15.c
    ffmpeg/libavcodec/mdct_fixed_32.c
    ffmpeg/libavcodec/mdct_float.c
    ffmpeg/libavcodec/me_cmp.c
    ffmpeg/libavcodec/mediacodec.c
    ffmpeg/libavcodec/mjpeg_parser.c
    ffmpeg/libavcodec/mjpegdec.c
    ffmpeg/libavcodec/mjpegdec_common.c
    ffmpeg/libavcodec/mjpegenc.c
    ffmpeg/libavcodec/mjpegenc_common.c
    ffmpeg/libavcodec/mjpegenc_huffman.c
    ffmpeg/libavcodec/motion_est.c
    ffmpeg/libavcodec/mpeg12data.c
    ffmpeg/libavcodec/mpeg12framerate.c
    ffmpeg/libavcodec/mpeg4audio.c
    ffmpeg/libavcodec/mpeg4video.c
    ffmpeg/libavcodec/mpeg4video_parser.c
    ffmpeg/libavcodec/mpeg4videodec.c
    ffmpeg/libavcodec/mpeg_er.c
    ffmpeg/libavcodec/mpegaudio.c
    ffmpeg/libavcodec/mpegaudio_parser.c
    ffmpeg/libavcodec/mpegaudiodata.c
    ffmpeg/libavcodec/mpegaudiodec_common.c
    ffmpeg/libavcodec/mpegaudiodec_fixed.c
    ffmpeg/libavcodec/mpegaudiodecheader.c
    ffmpeg/libavcodec/mpegaudiodsp.c
    ffmpeg/libavcodec/mpegaudiodsp_data.c
    ffmpeg/libavcodec/mpegaudiodsp_fixed.c
    ffmpeg/libavcodec/mpegaudiodsp_float.c
    ffmpeg/libavcodec/mpegpicture.c
    ffmpeg/libavcodec/mpegutils.c
    ffmpeg/libavcodec/mpegvideo.c
    ffmpeg/libavcodec/mpegvideo_enc.c
    ffmpeg/libavcodec/mpegvideo_motion.c
    ffmpeg/libavcodec/mpegvideodata.c
    ffmpeg/libavcodec/mpegvideodsp.c
    ffmpeg/libavcodec/mpegvideoencdsp.c
    ffmpeg/libavcodec/msmpeg4.c
    ffmpeg/libavcodec/msmpeg4data.c
    ffmpeg/libavcodec/msmpeg4dec.c
    ffmpeg/libavcodec/msrledec.c
    ffmpeg/libavcodec/null_bsf.c
    ffmpeg/libavcodec/options.c
    ffmpeg/libavcodec/opus.c
    ffmpeg/libavcodec/opus_celt.c
    ffmpeg/libavcodec/opus_pvq.c
    ffmpeg/libavcodec/opus_rc.c
    ffmpeg/libavcodec/opus_silk.c
    ffmpeg/libavcodec/opusdec.c
    ffmpeg/libavcodec/opusdsp.c
    ffmpeg/libavcodec/opustab.c
    ffmpeg/libavcodec/parser.c
    ffmpeg/libavcodec/parsers.c
    ffmpeg/libavcodec/pcm-bluray.c
    ffmpeg/libavcodec/pcm-dvd.c
    ffmpeg/libavcodec/pcm.c
    ffmpeg/libavcodec/pixblockdsp.c
    ffmpeg/libavcodec/png.c
    ffmpeg/libavcodec/pngdec.c
    ffmpeg/libavcodec/pngdsp.c
    ffmpeg/libavcodec/pngenc.c
    ffmpeg/libavcodec/profiles.c
    ffmpeg/libavcodec/pthread.c
    ffmpeg/libavcodec/pthread_frame.c
    ffmpeg/libavcodec/pthread_slice.c
    ffmpeg/libavcodec/qpeldsp.c
    ffmpeg/libavcodec/qsv_api.c
    ffmpeg/libavcodec/ratecontrol.c
    ffmpeg/libavcodec/raw.c
    ffmpeg/libavcodec/rdft.c
    ffmpeg/libavcodec/rl.c
    ffmpeg/libavcodec/sbrdsp.c
    ffmpeg/libavcodec/simple_idct.c
    ffmpeg/libavcodec/sinewin.c
    ffmpeg/libavcodec/srtdec.c
    ffmpeg/libavcodec/startcode.c
    ffmpeg/libavcodec/tiff.c
    ffmpeg/libavcodec/tiff_common.c
    ffmpeg/libavcodec/utils.c
    ffmpeg/libavcodec/vc1.c
    ffmpeg/libavcodec/vc1_block.c
    ffmpeg/libavcodec/vc1_loopfilter.c
    ffmpeg/libavcodec/vc1_mc.c
    ffmpeg/libavcodec/vc1_pred.c
    ffmpeg/libavcodec/vc1data.c
    ffmpeg/libavcodec/vc1dec.c
    ffmpeg/libavcodec/vc1dsp.c
    ffmpeg/libavcodec/videodsp.c
    ffmpeg/libavcodec/vorbis.c
    ffmpeg/libavcodec/vorbis_data.c
    ffmpeg/libavcodec/vorbis_parser.c
    ffmpeg/libavcodec/vorbisdec.c
    ffmpeg/libavcodec/vorbisdsp.c
    ffmpeg/libavcodec/vp3.c
    ffmpeg/libavcodec/vp3dsp.c
    ffmpeg/libavcodec/vp56rac.c
    ffmpeg/libavcodec/vp8.c
    ffmpeg/libavcodec/vp8dsp.c
    ffmpeg/libavcodec/vp9.c
    ffmpeg/libavcodec/vp9_parser.c
    ffmpeg/libavcodec/vp9_superframe_bsf.c
    ffmpeg/libavcodec/vp9_superframe_split_bsf.c
    ffmpeg/libavcodec/vp9block.c
    ffmpeg/libavcodec/vp9data.c
    ffmpeg/libavcodec/vp9dsp.c
    ffmpeg/libavcodec/vp9dsp_10bpp.c
    ffmpeg/libavcodec/vp9dsp_12bpp.c
    ffmpeg/libavcodec/vp9dsp_8bpp.c
    ffmpeg/libavcodec/vp9lpf.c
    ffmpeg/libavcodec/vp9mvs.c
    ffmpeg/libavcodec/vp9prob.c
    ffmpeg/libavcodec/vp9recon.c
    ffmpeg/libavcodec/webp.c
    ffmpeg/libavcodec/webvttdec.c
    ffmpeg/libavcodec/wmv2.c
    ffmpeg/libavcodec/wmv2data.c
    ffmpeg/libavcodec/wmv2dec.c
    ffmpeg/libavcodec/wmv2dsp.c
    ffmpeg/libavcodec/xiph.c
    ffmpeg/libavcodec/xsubdec.c
    ffmpeg/libavcodec/xvididct.c
)
target_link_libraries(avcodec PRIVATE ${FFMPEG_DEPENDENCIES} swscale swresample)
target_compile_definitions(avcodec PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(avcodec PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(avcodec 
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES} 
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(avformat
    ffmpeg/libavformat/ac3dec.c
    ffmpeg/libavformat/allformats.c
    ffmpeg/libavformat/av1.c
    ffmpeg/libavformat/avc.c
    ffmpeg/libavformat/avlanguage.c
    ffmpeg/libavformat/avidec.c
    ffmpeg/libavformat/avio.c
    ffmpeg/libavformat/aviobuf.c
    ffmpeg/libavformat/dump.c
    ffmpeg/libavformat/flac_picture.c
    ffmpeg/libavformat/flacdec.c
    ffmpeg/libavformat/flacenc.c
    ffmpeg/libavformat/flacenc_header.c
    ffmpeg/libavformat/format.c
    ffmpeg/libavformat/gifdec.c
    ffmpeg/libavformat/http.c
    ffmpeg/libavformat/httpauth.c
    ffmpeg/libavformat/hevc.c
    ffmpeg/libavformat/id3v1.c
    ffmpeg/libavformat/id3v2.c
    ffmpeg/libavformat/img2.c
    ffmpeg/libavformat/img2dec.c
    ffmpeg/libavformat/isom.c
    ffmpeg/libavformat/isom_tags.c
    ffmpeg/libavformat/matroska.c
    ffmpeg/libavformat/matroskadec.c
    ffmpeg/libavformat/matroskaenc.c
    ffmpeg/libavformat/metadata.c
    ffmpeg/libavformat/mov.c
    ffmpeg/libavformat/movenc.c
    ffmpeg/libavformat/movenccenc.c
    ffmpeg/libavformat/movenchint.c
    ffmpeg/libavformat/mov_chan.c
    ffmpeg/libavformat/mov_esds.c
    ffmpeg/libavformat/mp3dec.c
    ffmpeg/libavformat/mux.c
    ffmpeg/libavformat/network.c
    ffmpeg/libavformat/oggdec.c
    ffmpeg/libavformat/oggparsecelt.c
    ffmpeg/libavformat/oggparsedirac.c
    ffmpeg/libavformat/oggparseflac.c
    ffmpeg/libavformat/oggparseogm.c
    ffmpeg/libavformat/oggparseopus.c
    ffmpeg/libavformat/oggparseskeleton.c
    ffmpeg/libavformat/oggparsespeex.c
    ffmpeg/libavformat/oggparsetheora.c
    ffmpeg/libavformat/oggparsevorbis.c
    ffmpeg/libavformat/oggparsevp8.c
    ffmpeg/libavformat/options.c
    ffmpeg/libavformat/os_support.c
    ffmpeg/libavformat/pcm.c
    ffmpeg/libavformat/pcmdec.c
    ffmpeg/libavformat/protocols.c
    ffmpeg/libavformat/qtpalette.c
    ffmpeg/libavformat/rawdec.c
    ffmpeg/libavformat/rawutils.c
    ffmpeg/libavformat/replaygain.c
    ffmpeg/libavformat/riff.c
    ffmpeg/libavformat/riffdec.c
    ffmpeg/libavformat/riffenc.c
    ffmpeg/libavformat/rmsipr.c
    ffmpeg/libavformat/rtp.c
    ffmpeg/libavformat/rtpenc_chain.c
    ffmpeg/libavformat/sdp.c
    ffmpeg/libavformat/tcp.c
    ffmpeg/libavformat/url.c
    ffmpeg/libavformat/urldecode.c
    ffmpeg/libavformat/utils.c
    ffmpeg/libavformat/vorbiscomment.c
    ffmpeg/libavformat/vpcc.c
    ffmpeg/libavformat/wavdec.c
    ffmpeg/libavformat/wv.c
)
target_link_libraries(avformat PRIVATE ${FFMPEG_DEPENDENCIES} swscale avutil avcodec)
target_compile_definitions(avformat PRIVATE ${FFMPEG_DEFINITIONS})
target_compile_options(avformat PRIVATE ${FFMPEG_COMPILE_OPTIONS})
target_include_directories(avformat 
    PRIVATE ${FFMPEG_PRIVATE_INCLUDES} 
    PUBLIC ${FFMPEG_PUBLIC_INCLUDES}
)

add_library(avfilter
    ffmpeg/libavfilter/allfilters.c
    ffmpeg/libavfilter/audio.c
    ffmpeg/libavfilter/avfilter.c
    ffmpeg/libavfilter/avfiltergraph.c
    ffmpeg/libavfilter/buffersink.c
    ffmpeg/libavfilter/buffersrc.c
    ffmpeg/libavfilter/drawutils.c
    ffmpeg/libavfilter/fifo.c
    ffmpeg/libavfilter/formats.c
    ffmpeg/libavfilter/framepool.c
    ffmpeg/libavfilter/framequeue.c
    ffmpeg/libavfilter/graphdump.c
    ffmpeg/libavfilter/graphparser.c
    ffmpeg/libavfilter/pthread.c
    ffmpeg/libavfilter/scale_eval.c
    ffmpeg/libavfilter/transform.c
    ffmpeg/libavfilter/vf_scale.c
    ffmpeg/libavfilter/vf_thumbnail.c
    ffmpeg/libavfilter/video.c
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