#include "st7735s_log_display.h"

/*
 * Small-footprint fallback for STM32F401RCT6 (256KB flash).
 * Keeps call sites unchanged while omitting large bitmap font tables.
 */
int st7735s_log_display_init(void)
{
    return 0;
}

void st7735s_log_display_line(const char *text)
{
    (void)text;
}
