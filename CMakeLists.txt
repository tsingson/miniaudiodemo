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
               src/play_eq_linear.c
               src/play_eq_dynamic.c
               src/play_eq_normal.c
               src/play_multiband_dynamics.c
               src/miniaudio.c)
add_executable(play_mcu
               src/play_mcu.c
               src/play_dsp_common.c
               src/play_eq_linear.c
               src/play_eq_dynamic.c
               src/play_eq_normal.c
               src/play_multiband_dynamics.c)
target_include_directories(miniaudiodemo PRIVATE src)
target_include_directories(three_way_macos PRIVATE src)
target_include_directories(play_macos PRIVATE src)
target_include_directories(play_mcu PRIVATE src)

if(PLAY_USE_CMSIS_DSP)
  set(CMSIS_DSP_DIR "${CMAKE_SOURCE_DIR}/../CMSIS-DSP")
  if(EXISTS "${CMSIS_DSP_DIR}/CMakeLists.txt")
    set(HOST ON CACHE BOOL "Build CMSIS-DSP for host" FORCE)
    set(DISABLEFLOAT16 ON CACHE BOOL "Disable float16 kernels" FORCE)
    add_subdirectory(${CMSIS_DSP_DIR} ${CMAKE_BINARY_DIR}/cmsis-dsp)
    target_link_libraries(play_macos PRIVATE CMSISDSP)
    target_link_libraries(play_mcu PRIVATE CMSISDSP)
    target_compile_definitions(play_macos PRIVATE USE_CMSIS_DSP=1)
    target_compile_definitions(play_mcu PRIVATE USE_CMSIS_DSP=1)
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
