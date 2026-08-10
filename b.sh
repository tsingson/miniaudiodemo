
cmake -S . -B build-macos -DPLAY_TARGET_PLATFORM=macos -DPLAY_MACOS_USE_CMSIS_DSP=ON
cmake --build build-macos --target play_macos

