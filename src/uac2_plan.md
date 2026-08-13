新计划:

1. 读取 ./src/play_macos.c , 复制成 ./src/main_uac2_srv.c , 把这个原来是 macOS core audio 播放, 修改成通过 usb 的 uac2 发送给下面 uac2 的 stm32f401
2. 读取 ./src/main_pcm5102_st7735s.c , 复制为 src/main_pcm5102_st7735s_uac2.c , 修改为使用 zephyr 4.4.1 原生的 uac2 , 并且从 uac2 接收音频后, 转化为 pcm5102a 能播放的格式, 再推送给 pcm5102a 播放
3. 以上两个, 请分别提供快捷编译方式
4. 实现验证后, 请写一个文档 uac2_pcm5102_macos_dev_log.md, 详细记录开发技术细节, 及调试 过程中遇到的问题与解决