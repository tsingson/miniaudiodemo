#include "miniaudio.h"
#include <stdio.h>

int main()
{
    ma_result result;
    ma_context context;
    ma_engine engine;
    ma_sound sound;
    ma_loshelf_node eqNode;

    // 1. 定义允许使用的后端列表，这里只启用 CoreAudio
    ma_backend backends[] = {
        ma_backend_coreaudio
    };

    // 2. 初始化上下文（Context），传入我们指定的后端
    result = ma_context_init(backends, 1, NULL, &context);
    if (result != MA_SUCCESS)
    {
        printf("初始化 CoreAudio 上下文失败。\n");
        return -1;
    }

    // 3. 配置引擎参数，将上面创建的 context 绑定进去
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.pContext = &context;

    // 4. 初始化引擎
    result = ma_engine_init(&engineConfig, &engine);
    if (result != MA_SUCCESS)
    {
        printf("初始化强制 CoreAudio 引擎失败。\n");
        ma_context_uninit(&context);
        return -1;
    }

    printf("成功显式初始化 CoreAudio 播放引擎！\n");

    ma_uint32 channels   = ma_engine_get_channels(&engine);
    ma_uint32 sampleRate = ma_engine_get_sample_rate(&engine);

    // 5. 创建低架 EQ 节点，对 20-300Hz 频段提升 3dB
    ma_loshelf_node_config eqConfig = ma_loshelf_node_config_init(channels, sampleRate, 6.0, 1.0, 300.0);
    result = ma_loshelf_node_init(ma_engine_get_node_graph(&engine), &eqConfig, NULL, &eqNode);
    if (result != MA_SUCCESS)
    {
        printf("初始化 EQ 节点失败。\n");
        ma_engine_uninit(&engine);
        ma_context_uninit(&context);
        return -1;
    }

    // EQ 节点输出 -> 引擎终端
    ma_node_attach_output_bus(&eqNode, 0, ma_engine_get_endpoint(&engine), 0);

    // 6. 加载音频文件，不自动挂载到节点图
    result = ma_sound_init_from_file(&engine, "test.wav", MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT, NULL, NULL, &sound);
    if (result != MA_SUCCESS)
    {
        printf("加载音频文件失败。\n");
        ma_loshelf_node_uninit(&eqNode, NULL);
        ma_engine_uninit(&engine);
        ma_context_uninit(&context);
        return -1;
    }

    // 声音输出 -> EQ 节点输入
    ma_node_attach_output_bus(&sound, 0, &eqNode, 0);

    ma_sound_start(&sound);

    printf("正在播放（20-300Hz +3dB EQ）... 按回车键退出。\n");
    getchar();

    // 7. 释放资源
    ma_sound_uninit(&sound);
    ma_loshelf_node_uninit(&eqNode, NULL);
    ma_engine_uninit(&engine);
    ma_context_uninit(&context);

    return 0;
}
