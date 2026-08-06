#include "miniaudio.h"
#include <stdio.h>

int main()
{
    ma_result result;
    ma_context context;
    ma_engine engine;

    // 1. 定义允许使用的后端列表，这里只启用 CoreAudio
    ma_backend backends[] = {
        ma_backend_coreaudio
    };

    // 2. 初始化上下文（Context），传入我们指定的后端
    // 第一个参数是配置，传 NULL 使用默认配置
    // 后续参数用于限制只使用指定的后端数组
    result = ma_context_init(backends, 1, NULL, &context);
    if (result != MA_SUCCESS)
    {
        printf("初始化 CoreAudio 上下文失败。\n");
        return -1;
    }

    // 3. 配置引擎参数，将上面创建的 context 绑定进去
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.pContext = &context; // 显式绑定包含 CoreAudio 的上下文

    // 4. 初始化引擎
    result = ma_engine_init(&engineConfig, &engine);
    if (result != MA_SUCCESS)
    {
        printf("初始化强制 CoreAudio 引擎失败。\n");
        ma_context_uninit(&context);
        return -1;
    }

    printf("成功显式初始化 CoreAudio 播放引擎！\n");

    // 播放测试声音
    ma_engine_play_sound(&engine, "test.wav", NULL);

    printf("正在播放... 按回车键退出。\n");
    getchar();

    // 5. 释放资源（注意先释放引擎，再释放 context）
    ma_engine_uninit(&engine);
    ma_context_uninit(&context);

    return 0;
}
