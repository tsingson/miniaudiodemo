#include "st7735s_log_display.h"

#include <stdint.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#define GLYPH_BITMAP_W 5U
#define GLYPH_BITMAP_H 7U
#define GLYPH_W 8U
#define GLYPH_H 12U
#define CELL_W 8U
#define CELL_H 12U
#define CHAR_SPACING 0U
#define LINE_SPACING 2U

#define RGB565_WHITE 0xFFFFU
#define RGB565_BLACK 0x0000U

#if DT_HAS_CHOSEN(zephyr_display)
#define APP_DISPLAY_NODE DT_CHOSEN(zephyr_display)
#endif

#define ST7735S_CTRL_NODE DT_NODELABEL(st7735s_ctrl)

LOG_MODULE_REGISTER(st7735s_log_display_lite, LOG_LEVEL_INF);

struct glyph5x7 {
    char c;
    uint8_t row[GLYPH_H];
};

static const struct glyph5x7 g_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E}},
    {'6', {0x07, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x01, 0x02, 0x04, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'A', {0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'a', {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}},
    {'b', {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x1E}},
    {'c', {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E}},
    {'d', {0x01, 0x01, 0x0F, 0x11, 0x11, 0x11, 0x0F}},
    {'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},
    {'f', {0x06, 0x08, 0x08, 0x1E, 0x08, 0x08, 0x08}},
    {'g', {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}},
    {'h', {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}},
    {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
    {'j', {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}},
    {'k', {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}},
    {'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'m', {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15}},
    {'n', {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}},
    {'o', {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}},
    {'p', {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10}},
    {'q', {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01}},
    {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'s', {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}},
    {'t', {0x08, 0x08, 0x1E, 0x08, 0x08, 0x08, 0x06}},
    {'u', {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}},
    {'v', {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'w', {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}},
    {'x', {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}},
    {'y', {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E}},
    {'z', {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}},
};

static const struct device *g_display;
static struct display_capabilities g_caps;
static uint16_t g_cursor_y;
static uint8_t g_ready;

static const struct gpio_dt_spec g_st7735s_blk =
    GPIO_DT_SPEC_GET_OR(ST7735S_CTRL_NODE, blk_gpios, {0});

static void try_enable_blk_fallback(void)
{
#if defined(CONFIG_BOARD_NUCLEO_F401RE)
    const struct device *portb = DEVICE_DT_GET(DT_NODELABEL(gpiob));
    int ret;

    if (!device_is_ready(portb)) {
        LOG_WRN("BLK fallback gpioB not ready");
        return;
    }

    ret = gpio_pin_configure(portb, 1U, GPIO_OUTPUT_ACTIVE);
    LOG_INF("BLK fallback PB1 configure ret=%d", ret);
#endif
}

static const struct glyph5x7 *find_glyph(char c)
{
    size_t i;

    for (i = 0U; i < sizeof(g_font) / sizeof(g_font[0]); ++i) {
        if (g_font[i].c == c) {
            return &g_font[i];
        }
    }

    return &g_font[12]; /* '?' */
}

static int fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565)
{
    uint16_t linebuf[160];
    struct display_buffer_descriptor desc;
    uint16_t be = sys_cpu_to_be16(rgb565);
    uint16_t i;
    uint16_t row;

    if (!g_ready || w == 0U || h == 0U || w > (sizeof(linebuf) / sizeof(linebuf[0]))) {
        return -1;
    }

    for (i = 0U; i < w; ++i) {
        linebuf[i] = be;
    }

    desc.buf_size = (size_t)w * sizeof(uint16_t);
    desc.width = w;
    desc.height = 1U;
    desc.pitch = w;

    for (row = 0U; row < h; ++row) {
        int ret = display_write(g_display, x, y + row, &desc, linebuf);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int draw_glyph(uint16_t x, uint16_t y, const struct glyph5x7 *glyph)
{
    uint8_t gy;
    uint8_t gx;

    for (gy = 0U; gy < GLYPH_BITMAP_H; ++gy) {
        for (gx = 0U; gx < GLYPH_BITMAP_W; ++gx) {
            uint8_t bit = (uint8_t)(1U << (GLYPH_BITMAP_W - 1U - gx));
            if (glyph->row[gy] & bit) {
                int ret = fill_rect((uint16_t)(x + 1U + gx),
                                    (uint16_t)(y + 2U + gy),
                                    1U,
                                    1U,
                                    RGB565_BLACK);
                if (ret != 0) {
                    return ret;
                }
            }
        }
    }

    return 0;
}

/*
 * Small-footprint fallback for STM32F401RCT6 (256KB flash).
 * Keeps call sites unchanged while avoiding large bitmap font tables.
 */
int st7735s_log_display_init(void)
{
    if (g_ready) {
        return 0;
    }

#if DT_HAS_CHOSEN(zephyr_display)
    g_display = DEVICE_DT_GET(APP_DISPLAY_NODE);
#else
    return -1;
#endif

    if (!device_is_ready(g_display)) {
        LOG_ERR("display device not ready");
        return -1;
    }

    LOG_INF("display device ready");

    if (g_st7735s_blk.port == NULL) {
        LOG_WRN("BLK gpio not defined in DT");
        try_enable_blk_fallback();
    } else if (!device_is_ready(g_st7735s_blk.port)) {
        LOG_WRN("BLK gpio port not ready: pin=%u flags=0x%x", g_st7735s_blk.pin, g_st7735s_blk.dt_flags);
        try_enable_blk_fallback();
    } else {
        int blk_ret = gpio_pin_configure_dt(&g_st7735s_blk, GPIO_OUTPUT_ACTIVE);
        LOG_INF("BLK configure ret=%d pin=%u flags=0x%x", blk_ret, g_st7735s_blk.pin, g_st7735s_blk.dt_flags);
    }

    display_get_capabilities(g_display, &g_caps);
    LOG_INF("caps: %ux%u", g_caps.x_resolution, g_caps.y_resolution);
    (void)display_set_pixel_format(g_display, PIXEL_FORMAT_RGB_565);
    (void)display_blanking_off(g_display);

    g_cursor_y = 0U;
    g_ready = 1U;

    if (g_caps.x_resolution > 0U && g_caps.y_resolution > 0U) {
        (void)fill_rect(0U, 0U, g_caps.x_resolution, g_caps.y_resolution, RGB565_WHITE);
    }

    LOG_INF("display style: white background, black text");

    return 0;
}

void st7735s_log_display_line(const char *text)
{
    const char *p = text;
    uint16_t x = 0U;
    uint16_t y;
    uint16_t line_h = (uint16_t)(CELL_H + LINE_SPACING);

    if (!g_ready || text == NULL || g_caps.x_resolution == 0U || g_caps.y_resolution == 0U) {
        return;
    }

    if ((uint32_t)g_cursor_y + line_h > g_caps.y_resolution) {
        g_cursor_y = 0U;
    }

    y = g_cursor_y;
    (void)fill_rect(0U, y, g_caps.x_resolution, line_h, RGB565_WHITE);

    while (*p != '\0') {
        const struct glyph5x7 *glyph;

        if ((uint8_t)*p < 0x20U || (uint8_t)*p > 0x7EU) {
            glyph = &g_font[12];
            if (((uint8_t)*p & 0x80U) != 0U) {
                ++p;
                while (((uint8_t)*p & 0xC0U) == 0x80U) {
                    ++p;
                }
            } else {
                ++p;
            }
        } else {
            glyph = find_glyph(*p);
            ++p;
        }

        if ((uint32_t)x + CELL_W > g_caps.x_resolution) {
            break;
        }

        if (draw_glyph(x, y, glyph) != 0) {
            break;
        }

        x = (uint16_t)(x + CELL_W + CHAR_SPACING);
    }

    g_cursor_y = (uint16_t)(g_cursor_y + line_h);
}
