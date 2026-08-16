# UAC2 原始计划（已完成）

本文档保留最初任务背景；当前实现状态以项目 README、`play_uac2.md` 和 `uac2_pcm5102_macos_dev_log.md` 为准。

1. macOS 发送入口已实现为 `src/main_uac2_srv.c`，通过 CoreAudio/miniaudio 选择 STM32 UAC2 播放设备。
2. STM32F401 接收入口为 `src/main_pcm5102_stm32f401_uac2.c`，STM32H7B0 FK7B0M1 接收入口为 `src/main_pcm5102_stm32h7b0_uac2.c`；二者共享 `src/uac2/uac2_pcm5102_app.c`，使用 Zephyr 4.4.2 `device_next` UAC2。
3. 快捷构建/烧录命令已集中在 `r.sh`。
4. 硬件调试过程与后续研究方向见 `uac2_pcm5102_macos_dev_log.md`。