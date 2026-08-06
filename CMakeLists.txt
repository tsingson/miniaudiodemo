cmake_minimum_required(VERSION 3.20.0)
project(miniaudiodemo C)

set(CMAKE_C_STANDARD 11)

add_executable(miniaudiodemo src/main.c src/miniaudio.c)

add_executable(three_way_macos src/three_way_crossover_macos_offline.c
                               src/miniaudio.c)

add_executable(play_macos src/play_macos.c src/miniaudio.c)
target_include_directories(miniaudiodemo PRIVATE src)
target_include_directories(three_way_macos PRIVATE src)

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
