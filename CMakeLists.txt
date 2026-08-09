cmake_minimum_required(VERSION 3.20.0)
project(miniaudiodemo C)

set(CMAKE_C_STANDARD 11)
option(PLAY_USE_CMSIS_DSP "Use CMSIS-DSP in play_macos and play_mcu" ON)
if(DEFINED PLAY_MACOS_USE_CMSIS_DSP)
  set(PLAY_USE_CMSIS_DSP ${PLAY_MACOS_USE_CMSIS_DSP})
endif()

add_executable(miniaudiodemo src/main.c src/miniaudio.c)

add_executable(three_way_macos src/three_way_crossover_macos_offline.c
                               src/miniaudio.c)

add_executable(play_macos
               src/play_macos.c
               src/play_dsp_common.c
               src/play_pipeline.c
               src/play_pipeline_eq_stages.c
               src/play_eq_linear.c
               src/play_eq_dynamic.c
               src/play_eq_normal.c
               src/play_multiband_dynamics.c
               src/miniaudio.c)
add_executable(play_mcu
               src/play_mcu.c
               src/play_dsp_common.c
               src/play_pipeline.c
               src/play_pipeline_eq_stages.c
               src/play_eq_linear.c
               src/play_eq_dynamic.c
               src/play_eq_normal.c
               src/play_multiband_dynamics.c)
add_executable(verify_eq_gain
               src/verify_eq_gain.c
               src/play_dsp_common.c
               src/play_pipeline.c
               src/play_pipeline_eq_stages.c
               src/play_eq_linear.c
               src/play_eq_dynamic.c
               src/play_eq_normal.c
               src/play_multiband_dynamics.c)
add_executable(verify_eq_taps
               src/verify_eq_taps.c
               src/play_dsp_common.c
               src/play_pipeline.c
               src/play_pipeline_eq_stages.c
               src/play_eq_linear.c
               src/play_eq_dynamic.c
               src/play_eq_normal.c
               src/play_multiband_dynamics.c)
add_executable(test_eq_dynamic
               tests/test_eq_dynamic.c
               src/play_eq_dynamic.c)
add_executable(test_eq_normal
               tests/test_eq_normal.c
               src/play_eq_normal.c)
add_executable(test_eq_linear
               tests/test_eq_linear.c
               src/play_eq_linear.c)
add_executable(test_pipeline_chain
               tests/test_pipeline_chain.c
               src/play_pipeline.c
               src/play_pipeline_eq_stages.c
               src/play_eq_linear.c
               src/play_eq_dynamic.c
               src/play_eq_normal.c)
target_include_directories(miniaudiodemo PRIVATE src)
target_include_directories(three_way_macos PRIVATE src)
target_include_directories(play_macos PRIVATE src)
target_include_directories(play_mcu PRIVATE src)
target_include_directories(verify_eq_gain PRIVATE src)
target_include_directories(verify_eq_taps PRIVATE src)
target_include_directories(test_eq_dynamic PRIVATE src)
target_include_directories(test_eq_normal PRIVATE src)
target_include_directories(test_eq_linear PRIVATE src)
target_include_directories(test_pipeline_chain PRIVATE src)

if(PLAY_USE_CMSIS_DSP)
  set(CMSIS_DSP_DIR "${CMAKE_SOURCE_DIR}/../CMSIS-DSP")
  if(EXISTS "${CMSIS_DSP_DIR}/CMakeLists.txt")
    set(HOST ON CACHE BOOL "Build CMSIS-DSP for host" FORCE)
    set(DISABLEFLOAT16 ON CACHE BOOL "Disable float16 kernels" FORCE)
    add_subdirectory(${CMSIS_DSP_DIR} ${CMAKE_BINARY_DIR}/cmsis-dsp)
    target_link_libraries(play_macos PRIVATE CMSISDSP)
    target_link_libraries(play_mcu PRIVATE CMSISDSP)
    target_link_libraries(verify_eq_gain PRIVATE CMSISDSP)
    target_link_libraries(verify_eq_taps PRIVATE CMSISDSP)
    target_link_libraries(test_eq_linear PRIVATE CMSISDSP)
    target_compile_definitions(play_macos PRIVATE USE_CMSIS_DSP=1)
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
elseif(UNIX)
  target_link_libraries(play_macos PRIVATE pthread m dl)
endif()

if(APPLE)
  target_link_libraries(verify_eq_gain PRIVATE m)
  target_link_libraries(verify_eq_taps PRIVATE m)
  target_link_libraries(test_eq_dynamic PRIVATE m)
  target_link_libraries(test_eq_normal PRIVATE m)
  target_link_libraries(test_eq_linear PRIVATE m)
  target_link_libraries(test_pipeline_chain PRIVATE m)
elseif(UNIX)
  target_link_libraries(verify_eq_gain PRIVATE pthread m dl)
  target_link_libraries(verify_eq_taps PRIVATE pthread m dl)
  target_link_libraries(test_eq_dynamic PRIVATE pthread m dl)
  target_link_libraries(test_eq_normal PRIVATE pthread m dl)
  target_link_libraries(test_eq_linear PRIVATE pthread m dl)
  target_link_libraries(test_pipeline_chain PRIVATE pthread m dl)
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
target_compile_options(play_mcu PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(verify_eq_gain PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(verify_eq_taps PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_eq_dynamic PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_eq_normal PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_eq_linear PRIVATE -Wall -Wno-unused-parameter)
target_compile_options(test_pipeline_chain PRIVATE -Wall -Wno-unused-parameter)

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

add_custom_target(run_eq_unit_tests
  COMMAND ${CMAKE_SOURCE_DIR}/test_eq_dynamic
  COMMAND ${CMAKE_SOURCE_DIR}/test_eq_normal
  COMMAND ${CMAKE_SOURCE_DIR}/test_eq_linear
  COMMAND ${CMAKE_SOURCE_DIR}/test_pipeline_chain
  DEPENDS test_eq_dynamic test_eq_normal test_eq_linear test_pipeline_chain)
