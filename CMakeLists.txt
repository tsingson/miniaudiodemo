cmake_minimum_required(VERSION 3.20.0)

if(DEFINED ZEPHYR_BASE OR DEFINED ENV{ZEPHYR_BASE})
  find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
  project(miniaudiodemo_zephyr)

  option(MINIAUDIO_ST7735S_ZPIX_FONT "Enable full ZPix Chinese font logger (large flash usage)" OFF)
  option(MINIAUDIO_ST7735S_DEMO_MAIN "Use minimal ST7735S hello-copilot demo entry" OFF)
  option(MINIAUDIO_PCM5102_ST7735S_MAIN "Use PCM5102A + ST7735S audio demo entry" OFF)
  option(MINIAUDIO_PCM5102_ST7735S_UAC2_MAIN "Use UAC2 receiver + PCM5102A + ST7735S entry" OFF)
  option(MINIAUDIO_PCM5102A_TEST_MAIN "Use deterministic PCM5102A writer test entry" OFF)
  option(MINIAUDIO_I2S_MAIN "Use direct PCM5102 I2S WAV playback entry" OFF)
  option(MINIAUDIO_STM32H7B0VBT6_DEMO_MAIN "Use minimal STM32H7B0VBT6 hello-serial demo entry" OFF)

  if(MINIAUDIO_PCM5102A_TEST_MAIN)
    target_sources(app PRIVATE
      src/main_pcm5102a_test.c
      src/audio_4_wav_data.c
      src/pcm5102a_audio.c
      src/play_audio_pipeline.c
      src/dspeq/play_dsp_common.c
      src/dspeq/play_dsp_plugins.c
      src/dspeq/play_eq_runtime.c
      src/dspeq/eq_low_seq.c
      src/dspeq/play_pipeline.c
      src/dspeq/play_pipeline_eq_stages.c
      src/dspeq/play_eq_linear.c
      src/dspeq/play_eq_dynamic.c
      src/dspeq/play_eq_normal.c
      src/dspeq/play_multiband_dynamics.c)
  elseif(MINIAUDIO_STM32H7B0VBT6_DEMO_MAIN)
    target_sources(app PRIVATE src/main_stm32h7b0vbt6.c)
  elseif(MINIAUDIO_PCM5102_ST7735S_UAC2_MAIN)
    target_sources(app PRIVATE
      src/main_pcm5102_st7735s_uac2.c
      src/play_mcu_zephyr_pcm5102.c
      src/pcm5102a_audio.c
      src/play_audio_pipeline.c
      src/dspeq/play_dsp_common.c
      src/dspeq/play_dsp_plugins.c
      src/dspeq/play_eq_runtime.c
      src/dspeq/eq_low_seq.c
      src/dspeq/play_pipeline.c
      src/dspeq/play_pipeline_eq_stages.c
      src/dspeq/play_eq_linear.c
      src/dspeq/play_eq_dynamic.c
      src/dspeq/play_eq_normal.c
      src/dspeq/play_multiband_dynamics.c
      src/audio_3_wav_data.c
      src/usb_uac2_device.c)
  elseif(MINIAUDIO_PCM5102_ST7735S_MAIN)
    target_sources(app PRIVATE
      src/main_pcm5102_st7735s.c
      src/audio_3_wav_data.c
      src/pcm5102a_audio.c
      src/play_audio_pipeline.c
      src/dspeq/play_dsp_common.c
      src/dspeq/play_dsp_plugins.c
      src/dspeq/play_eq_runtime.c
      src/dspeq/eq_low_seq.c
      src/dspeq/play_pipeline.c
      src/dspeq/play_pipeline_eq_stages.c
      src/dspeq/play_eq_linear.c
      src/dspeq/play_eq_dynamic.c
      src/dspeq/play_eq_normal.c
      src/dspeq/play_multiband_dynamics.c
      src/play_mcu_zephyr_pcm5102.c)
  elseif(MINIAUDIO_I2S_MAIN)
    target_sources(app PRIVATE src/main_i2s.c src/wav_3.c)
  elseif(MINIAUDIO_ST7735S_DEMO_MAIN)
    target_sources(app PRIVATE src/main_st7735s.c)
  else()
    target_sources(app PRIVATE
      src/play_mcu.c
      src/play_mcu_zephyr_pcm5102.c
      src/pcm5102a_audio.c
      src/play_audio_pipeline.c
      src/dspeq/play_dsp_common.c
      src/dspeq/play_dsp_plugins.c
      src/dspeq/play_eq_runtime.c
      src/dspeq/eq_low_seq.c
      src/dspeq/play_pipeline.c
      src/dspeq/play_pipeline_eq_stages.c
      src/dspeq/play_eq_linear.c
      src/dspeq/play_eq_dynamic.c
      src/dspeq/play_eq_normal.c
      src/dspeq/play_multiband_dynamics.c)
  endif()

  if(MINIAUDIO_ST7735S_ZPIX_FONT)
    target_sources(app PRIVATE
      src/st7735s_log_display.c
      src/zpix12_font_data.c)
    target_compile_definitions(app PRIVATE MINIAUDIO_ST7735S_ZPIX_FONT=1)
  else()
    target_sources(app PRIVATE src/st7735s_log_display_stub.c)
  endif()

  target_include_directories(app PRIVATE src src/dspeq)
  target_compile_options(app PRIVATE -Wall -Wno-unused-parameter)
else()
project(miniaudiodemo C)

set(CMAKE_C_STANDARD 11)
option(PLAY_USE_CMSIS_DSP "Use CMSIS-DSP in play_macos and play_mcu" ON)
if(DEFINED PLAY_MACOS_USE_CMSIS_DSP)
  set(PLAY_USE_CMSIS_DSP ${PLAY_MACOS_USE_CMSIS_DSP})
endif()

add_executable(miniaudiodemo src/main.c src/miniaudio/miniaudio.c)

add_executable(three_way_macos src/three_way_crossover_macos_offline.c
                               src/miniaudio/miniaudio.c)

add_executable(play_macos
               src/play_macos.c
               src/dsp_http_ctrl.c
               src/dspeq/play_dsp_common.c
               src/dspeq/play_dsp_plugins.c
               src/dspeq/play_eq_runtime.c
               src/dspeq/eq_low_seq.c
               src/dspeq/play_pipeline.c
               src/dspeq/play_pipeline_eq_stages.c
               src/dspeq/play_eq_linear.c
               src/dspeq/play_eq_dynamic.c
               src/dspeq/play_eq_normal.c
               src/dspeq/play_multiband_dynamics.c
               src/miniaudio/miniaudio.c)
add_executable(main_uac2_srv
               src/main_uac2_srv.c
               src/dsp_http_ctrl.c
               src/dspeq/play_dsp_common.c
               src/dspeq/play_dsp_plugins.c
               src/dspeq/play_eq_runtime.c
               src/dspeq/eq_low_seq.c
               src/dspeq/play_pipeline.c
               src/dspeq/play_pipeline_eq_stages.c
               src/dspeq/play_eq_linear.c
               src/dspeq/play_eq_dynamic.c
               src/dspeq/play_eq_normal.c
               src/dspeq/play_multiband_dynamics.c
               src/miniaudio/miniaudio.c)
add_executable(play_mcu
               src/play_mcu.c
               src/dspeq/play_dsp_common.c
               src/dspeq/play_dsp_plugins.c
               src/dspeq/play_eq_runtime.c
               src/dspeq/eq_low_seq.c
               src/dspeq/play_pipeline.c
               src/dspeq/play_pipeline_eq_stages.c
               src/dspeq/play_eq_linear.c
               src/dspeq/play_eq_dynamic.c
               src/dspeq/play_eq_normal.c
               src/dspeq/play_multiband_dynamics.c)
add_executable(verify_eq_gain
               tests/verify_eq_gain.c
               src/dspeq/play_dsp_common.c
               src/dspeq/play_dsp_plugins.c
               src/dspeq/play_eq_runtime.c
               src/dspeq/eq_low_seq.c
               src/dspeq/play_pipeline.c
               src/dspeq/play_pipeline_eq_stages.c
               src/dspeq/play_eq_linear.c
               src/dspeq/play_eq_dynamic.c
               src/dspeq/play_eq_normal.c
               src/dspeq/play_multiband_dynamics.c)
add_executable(verify_eq_taps
               tests/verify_eq_taps.c
               src/dspeq/play_dsp_common.c
               src/dspeq/play_dsp_plugins.c
               src/dspeq/play_eq_runtime.c
               src/dspeq/eq_low_seq.c
               src/dspeq/play_pipeline.c
               src/dspeq/play_pipeline_eq_stages.c
               src/dspeq/play_eq_linear.c
               src/dspeq/play_eq_dynamic.c
               src/dspeq/play_eq_normal.c
               src/dspeq/play_multiband_dynamics.c)
add_executable(verify_eq_linear_mono
               tests/verify_eq_linear_mono.c
               src/dspeq/play_eq_linear.c
               src/miniaudio/miniaudio.c)
add_executable(verify_eq_low_seq
               tests/test_eq_low_seq.c
               src/dspeq/eq_low_seq.c)
add_executable(test_eq_dynamic
               tests/test_eq_dynamic.c
               src/dspeq/play_eq_dynamic.c)
add_executable(test_eq_normal
               tests/test_eq_normal.c
               src/dspeq/play_eq_normal.c)
add_executable(test_eq_linear
               tests/test_eq_linear.c
               src/dspeq/play_eq_linear.c)
add_executable(test_pipeline_chain
               tests/test_pipeline_chain.c
               src/dspeq/play_pipeline.c
               src/dspeq/play_pipeline_eq_stages.c
               src/dspeq/play_eq_runtime.c
               src/dspeq/eq_low_seq.c
               src/dspeq/play_eq_linear.c
               src/dspeq/play_eq_dynamic.c
               src/dspeq/play_eq_normal.c
               src/dspeq/play_multiband_dynamics.c)
add_executable(test_channel_plan
               tests/test_channel_plan.c
               src/play_channel_plan.c)
target_include_directories(miniaudiodemo PRIVATE src src/dspeq)
target_include_directories(three_way_macos PRIVATE src src/dspeq)
target_include_directories(play_macos PRIVATE src src/dspeq)
target_include_directories(main_uac2_srv PRIVATE src src/dspeq)
target_include_directories(play_mcu PRIVATE src src/dspeq)
target_include_directories(verify_eq_gain PRIVATE src src/dspeq)
target_include_directories(verify_eq_taps PRIVATE src src/dspeq)
target_include_directories(verify_eq_linear_mono PRIVATE src src/dspeq)
target_include_directories(verify_eq_low_seq PRIVATE src src/dspeq)
target_include_directories(test_eq_dynamic PRIVATE src src/dspeq)
target_include_directories(test_eq_normal PRIVATE src src/dspeq)
target_include_directories(test_eq_linear PRIVATE src src/dspeq)
target_include_directories(test_pipeline_chain PRIVATE src src/dspeq)
target_include_directories(test_channel_plan PRIVATE src src/dspeq)

if(PLAY_USE_CMSIS_DSP)
  set(CMSIS_DSP_DIR "${CMAKE_SOURCE_DIR}/../CMSIS-DSP")
  if(EXISTS "${CMSIS_DSP_DIR}/CMakeLists.txt")
    set(HOST ON CACHE BOOL "Build CMSIS-DSP for host" FORCE)
    set(DISABLEFLOAT16 ON CACHE BOOL "Disable float16 kernels" FORCE)
    add_subdirectory(${CMSIS_DSP_DIR} ${CMAKE_BINARY_DIR}/cmsis-dsp)
    target_link_libraries(play_macos PRIVATE CMSISDSP)
    target_link_libraries(main_uac2_srv PRIVATE CMSISDSP)
    target_link_libraries(play_mcu PRIVATE CMSISDSP)
    target_link_libraries(verify_eq_gain PRIVATE CMSISDSP)
    target_link_libraries(verify_eq_taps PRIVATE CMSISDSP)
    target_link_libraries(verify_eq_linear_mono PRIVATE CMSISDSP)
    target_link_libraries(verify_eq_low_seq PRIVATE CMSISDSP)
    target_link_libraries(test_eq_linear PRIVATE CMSISDSP)
    target_compile_definitions(play_macos PRIVATE USE_CMSIS_DSP=1)
    target_compile_definitions(main_uac2_srv PRIVATE USE_CMSIS_DSP=1)
    target_compile_definitions(play_mcu PRIVATE USE_CMSIS_DSP=1)
    target_compile_definitions(verify_eq_gain PRIVATE USE_CMSIS_DSP=1)
    target_compile_definitions(verify_eq_taps PRIVATE USE_CMSIS_DSP=1)
    target_compile_definitions(test_eq_linear PRIVATE USE_CMSIS_DSP=1)
  else()
    message(WARNING "CMSIS-DSP not found at ${CMSIS_DSP_DIR}; play_macos/play_mcu will use libc math fallback.")
  endif()
endif()

if(APPLE)
  target_link_libraries(
    miniaudiodemo PRIVATE "-framework CoreAudio" "-framework CoreFoundation"
                          "-framework AudioToolbox")
elseif(UNIX)
  target_link_libraries(miniaudiodemo PRIVATE pthread m dl)
endif()

if(APPLE)
  target_link_libraries(
    play_macos PRIVATE "-framework CoreAudio" "-framework CoreFoundation"
                       "-framework AudioToolbox")
  target_link_libraries(
    main_uac2_srv PRIVATE "-framework CoreAudio" "-framework CoreFoundation"
                         "-framework AudioToolbox")
elseif(UNIX)
  target_link_libraries(play_macos PRIVATE pthread m dl)
  target_link_libraries(main_uac2_srv PRIVATE pthread m dl)
endif()

if(APPLE)
  target_link_libraries(verify_eq_gain PRIVATE m)
  target_link_libraries(verify_eq_taps PRIVATE m)
  target_link_libraries(
    verify_eq_linear_mono PRIVATE "-framework CoreAudio" "-framework CoreFoundation"
                                 "-framework AudioToolbox")
  target_link_libraries(verify_eq_low_seq PRIVATE m)
  target_link_libraries(test_eq_dynamic PRIVATE m)
  target_link_libraries(test_eq_normal PRIVATE m)
  target_link_libraries(test_eq_linear PRIVATE m)
  target_link_libraries(test_pipeline_chain PRIVATE m)
  target_link_libraries(test_channel_plan PRIVATE m)
elseif(UNIX)
  target_link_libraries(verify_eq_gain PRIVATE pthread m dl)
  target_link_libraries(verify_eq_taps PRIVATE pthread m dl)
  target_link_libraries(verify_eq_linear_mono PRIVATE pthread m dl)
  target_link_libraries(verify_eq_low_seq PRIVATE pthread m dl)
  target_link_libraries(test_eq_dynamic PRIVATE pthread m dl)
  target_link_libraries(test_eq_normal PRIVATE pthread m dl)
  target_link_libraries(test_eq_linear PRIVATE pthread m dl)
  target_link_libraries(test_pipeline_chain PRIVATE pthread m dl)
  target_link_libraries(test_channel_plan PRIVATE pthread m dl)
endif()

if(APPLE)
  target_link_libraries(
    three_way_macos PRIVATE "-framework CoreAudio" "-framework CoreFoundation"
                            "-framework AudioToolbox")
elseif(UNIX)
  target_link_libraries(three_way_macos PRIVATE pthread m dl)
endif()

target_compile_options(miniaudiodemo PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(three_way_macos PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(play_macos PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(main_uac2_srv PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(play_mcu PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(verify_eq_gain PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(verify_eq_taps PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(verify_eq_linear_mono PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(verify_eq_low_seq PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_eq_dynamic PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_eq_normal PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_eq_linear PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_pipeline_chain PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_channel_plan PRIVATE -Wall -Wno-unused-parameter)

# 编译后把可执行文件复制到项目根目录，方便直接运行（test.wav 也在根目录）
add_custom_command(
  TARGET miniaudiodemo
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:miniaudiodemo>
          ${CMAKE_SOURCE_DIR}/miniaudiodemo
  COMMENT "Copied miniaudiodemo to project root")
# 编译后把可执行文件复制到项目根目录，方便直接运行（test.wav 也在根目录）
add_custom_command(
  TARGET three_way_macos
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:three_way_macos>
          ${CMAKE_SOURCE_DIR}/three_way_macos
  COMMENT "Copied three_way_macos to project root")

# 编译后把可执行文件复制到项目根目录，方便直接运行（test.wav 也在根目录）
add_custom_command(
  TARGET play_macos
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:play_macos>
          ${CMAKE_SOURCE_DIR}/play_macos
  COMMENT "Copied play_macos to project root")

add_custom_command(
  TARGET play_mcu
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:play_mcu>
    ${CMAKE_SOURCE_DIR}/play_mcu
  COMMENT "Copied play_mcu to project root")

add_custom_command(
  TARGET verify_eq_gain
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:verify_eq_gain>
    ${CMAKE_SOURCE_DIR}/verify_eq_gain
  COMMENT "Copied verify_eq_gain to project root")

add_custom_command(
  TARGET verify_eq_taps
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:verify_eq_taps>
    ${CMAKE_SOURCE_DIR}/verify_eq_taps
  COMMENT "Copied verify_eq_taps to project root")

add_custom_command(
  TARGET verify_eq_linear_mono
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:verify_eq_linear_mono>
    ${CMAKE_SOURCE_DIR}/verify_eq_linear_mono
  COMMENT "Copied verify_eq_linear_mono to project root")

add_custom_command(
  TARGET verify_eq_low_seq
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:verify_eq_low_seq>
    ${CMAKE_SOURCE_DIR}/verify_eq_low_seq
  COMMENT "Copied verify_eq_low_seq to project root")

add_custom_command(
  TARGET test_eq_dynamic
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:test_eq_dynamic>
    ${CMAKE_SOURCE_DIR}/test_eq_dynamic
  COMMENT "Copied test_eq_dynamic to project root")

add_custom_command(
  TARGET test_eq_normal
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:test_eq_normal>
    ${CMAKE_SOURCE_DIR}/test_eq_normal
  COMMENT "Copied test_eq_normal to project root")

add_custom_command(
  TARGET test_eq_linear
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:test_eq_linear>
    ${CMAKE_SOURCE_DIR}/test_eq_linear
  COMMENT "Copied test_eq_linear to project root")

add_custom_command(
  TARGET test_pipeline_chain
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:test_pipeline_chain>
    ${CMAKE_SOURCE_DIR}/test_pipeline_chain
  COMMENT "Copied test_pipeline_chain to project root")

add_custom_command(
  TARGET test_channel_plan
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:test_channel_plan>
    ${CMAKE_SOURCE_DIR}/test_channel_plan
  COMMENT "Copied test_channel_plan to project root")

add_custom_target(run_eq_unit_tests
  COMMAND ${CMAKE_SOURCE_DIR}/verify_eq_low_seq
  COMMAND ${CMAKE_SOURCE_DIR}/test_eq_dynamic
  COMMAND ${CMAKE_SOURCE_DIR}/test_eq_normal
  COMMAND ${CMAKE_SOURCE_DIR}/test_eq_linear
  COMMAND ${CMAKE_SOURCE_DIR}/test_pipeline_chain
  COMMAND ${CMAKE_SOURCE_DIR}/test_channel_plan
  DEPENDS verify_eq_low_seq test_eq_dynamic test_eq_normal test_eq_linear test_pipeline_chain test_channel_plan)
endif()
