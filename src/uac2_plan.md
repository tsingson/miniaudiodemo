# UAC2 原始计划（已完成）

本文档保留最初任务背景；当前实现状态以项目 README、`play_uac2.md` 和 `uac2_pcm5102_macos_dev_log.md` 为准。

1. macOS 发送入口已实现为 `src/main_uac2_srv.c`，通过 CoreAudio/miniaudio 选择 STM32 UAC2 播放设备。
2. STM32F401 接收入口为 `src/main_pcm5102_st7735s_uac2.c`，使用 Zephyr 4.4.2 `device_next` UAC2；可复用协议/流桥接位于 `src/uac2/`，PCM5102A 输出位于 `src/pcm5102/`。
3. 快捷构建/烧录命令已集中在 `r.sh`。
4. 硬件调试过程与后续研究方向见 `uac2_pcm5102_macos_dev_log.md`。