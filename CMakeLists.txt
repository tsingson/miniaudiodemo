cmake_minimum_required(VERSION 3.20.0)
project(miniaudiodemo C)

add_executable(miniaudiodemo
    src/main.c
    src/miniaudio.c
)

target_include_directories(miniaudiodemo PRIVATE src)

# macOS requires these frameworks for audio output
if(APPLE)
    target_link_libraries(miniaudiodemo PRIVATE
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework AudioToolbox"
    )
endif()

target_compile_options(miniaudiodemo PRIVATE -Wall -Wno-unused-parameter)
