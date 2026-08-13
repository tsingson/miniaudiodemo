#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "st7735s_log_display.h"

LOG_MODULE_REGISTER(main_st7735s, LOG_LEVEL_INF);

void main(void)
{
    int ret = st7735s_log_display_init();

    LOG_INF("main_st7735s start");

    if (ret != 0) {
        LOG_ERR("ST7735S logger init failed: %d", ret);
    } else {
        st7735s_log_display_line("Hello Copilot");
        st7735s_log_display_line("ST7735S 8x12 ASCII");
        LOG_INF("ST7735S text rendered");
    }

    while (1) {
        k_sleep(K_SECONDS(1));
    }
}
