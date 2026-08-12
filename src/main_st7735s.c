#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#define GLYPH_W 5U
#define GLYPH_H 7U
#define SCALE 1U
#define GLYPH_SPACING 1U
#define CELL_H 8U

#define RGB565_WHITE 0xFFFFU
#define RGB565_BLACK 0x0000U

#if DT_HAS_CHOSEN(zephyr_display)
#define APP_DISPLAY_NODE DT_CHOSEN(zephyr_display)
#endif

#define ST7735S_CTRL_NODE DT_NODELABEL(st7735s_ctrl)

LOG_MODULE_REGISTER(main_st7735s, LOG_LEVEL_INF);

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

struct glyph5x7 {
    char c;
    uint8_t row[GLYPH_H];
};

static const struct glyph5x7 g_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'c', {0x00, 0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E}},
    {'e', {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}},
    {'h', {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}},
    {'i', {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}},
    {'l', {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'o', {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}},
    {'p', {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10}},
    {'t', {0x04, 0x04, 0x1F, 0x04, 0x04, 0x04, 0x03}},
};

static const struct glyph5x7 *find_glyph(char c)
{
    size_t i;

    for (i = 0U; i < sizeof(g_font) / sizeof(g_font[0]); ++i) {
        if (g_font[i].c == c) {
            return &g_font[i];
        }
    }

    return &g_font[0];
}

static int draw_rect(const struct device *display,
                     uint16_t x,
                     uint16_t y,
                     uint16_t w,
                     uint16_t h,
                     uint16_t rgb565)
{
    uint16_t linebuf[160];
    struct display_buffer_descriptor desc;
    uint16_t be = sys_cpu_to_be16(rgb565);
    uint16_t i;
    uint16_t row;

    if (w == 0U || h == 0U || w > (sizeof(linebuf) / sizeof(linebuf[0]))) {
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
        int ret = display_write(display, x, y + row, &desc, linebuf);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int draw_glyph_scaled(const struct device *display,
                             const struct glyph5x7 *glyph,
                             uint16_t x,
                             uint16_t y,
                             uint16_t scale)
{
    uint8_t gy;
    uint8_t gx;

    for (gy = 0U; gy < GLYPH_H; ++gy) {
        for (gx = 0U; gx < GLYPH_W; ++gx) {
            uint8_t bit = (uint8_t)(1U << (GLYPH_W - 1U - gx));
            if (glyph->row[gy] & bit) {
                int ret = draw_rect(display,
                                   (uint16_t)(x + gx * scale),
                                   (uint16_t)(y + gy * scale),
                                   scale,
                                   scale,
                                   RGB565_BLACK);
                if (ret != 0) {
                    return ret;
                }
            }
        }
    }

    return 0;
}

void main(void)
{
#if DT_HAS_CHOSEN(zephyr_display)
    const struct device *display = DEVICE_DT_GET(APP_DISPLAY_NODE);
    const struct gpio_dt_spec blk = GPIO_DT_SPEC_GET_OR(ST7735S_CTRL_NODE, blk_gpios, {0});
    struct display_capabilities caps;
    const char *msg = "hello copilot";
    uint16_t text_w = (uint16_t)(strlen(msg) * (GLYPH_W * SCALE + GLYPH_SPACING));
    uint16_t text_h = CELL_H;
    uint16_t x;
    uint16_t y;
    int ret;

    LOG_INF("main_st7735s start");

    if (!device_is_ready(display)) {
        LOG_ERR("display device not ready");
        while (1) {
            k_sleep(K_MSEC(1000));
        }
    }

    LOG_INF("display device ready");

    if (blk.port == NULL) {
        LOG_WRN("BLK gpio not defined in DT");
        try_enable_blk_fallback();
    } else if (!device_is_ready(blk.port)) {
        LOG_WRN("BLK gpio port not ready: pin=%u flags=0x%x", blk.pin, blk.dt_flags);
        try_enable_blk_fallback();
    } else {
        ret = gpio_pin_configure_dt(&blk, GPIO_OUTPUT_ACTIVE);
        LOG_INF("BLK configure ret=%d pin=%u flags=0x%x", ret, blk.pin, blk.dt_flags);
    }

    display_get_capabilities(display, &caps);
    LOG_INF("display caps: %ux%u", caps.x_resolution, caps.y_resolution);

    ret = display_set_pixel_format(display, PIXEL_FORMAT_RGB_565);
    LOG_INF("display_set_pixel_format ret=%d", ret);

    ret = display_blanking_off(display);
    LOG_INF("display_blanking_off ret=%d", ret);

    if (draw_rect(display, 0U, 0U, caps.x_resolution, caps.y_resolution, RGB565_WHITE) != 0) {
        LOG_ERR("draw white background failed");
        while (1) {
            k_sleep(K_MSEC(250));
        }
    }

    LOG_INF("white background drawn");

    x = (caps.x_resolution > text_w) ? (uint16_t)((caps.x_resolution - text_w) / 2U) : 0U;
    y = (caps.y_resolution > text_h) ? (uint16_t)((caps.y_resolution - text_h) / 2U) : 0U;

    while (*msg != '\0') {
        const struct glyph5x7 *glyph = find_glyph(*msg);
        if (draw_glyph_scaled(display, glyph, x, y, SCALE) != 0) {
            LOG_ERR("draw glyph failed at char '%c'", *msg);
            while (1) {
                k_sleep(K_MSEC(250));
            }
        }
        x = (uint16_t)(x + GLYPH_W * SCALE + GLYPH_SPACING);
        ++msg;
    }

    LOG_INF("hello copilot rendered");

    while (1) {
        k_sleep(K_SECONDS(1));
    }
#else
    LOG_ERR("no zephyr,display chosen node");
    while (1) {
        k_sleep(K_MSEC(1000));
    }
#endif
}
