#include "st7735s_log_display.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "zpix12_font_data.h"

#define GLYPH_PX 12U
#define LINE_SPACING 2U
#define CHAR_SPACING 1U

#if DT_HAS_CHOSEN(zephyr_display)
#define APP_DISPLAY_NODE DT_CHOSEN(zephyr_display)
#endif

#define ST7735S_CTRL_NODE DT_NODELABEL(st7735s_ctrl)

LOG_MODULE_REGISTER(st7735s_log_display, LOG_LEVEL_INF);

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

static const struct device *g_display;
static struct display_capabilities g_caps;
static uint16_t g_fg = 0x0000;
static uint16_t g_bg = 0xFFFF;
static uint16_t g_cursor_y;
static uint8_t g_ready;
static const struct gpio_dt_spec g_st7735s_blk = GPIO_DT_SPEC_GET_OR(ST7735S_CTRL_NODE, blk_gpios, {0});

static const zpix12_glyph_t *find_glyph(uint32_t codepoint)
{
    size_t left = 0U;
    size_t right = zpix12_font_glyphs_count;

    while (left < right) {
        size_t mid = left + ((right - left) / 2U);
        uint32_t cp = zpix12_font_glyphs[mid].codepoint;

        if (cp == codepoint) {
            return &zpix12_font_glyphs[mid];
        }

        if (cp < codepoint) {
            left = mid + 1U;
        } else {
            right = mid;
        }
    }

    return NULL;
}

static uint8_t utf8_decode_next(const char **text, uint32_t *codepoint)
{
    const uint8_t *s = (const uint8_t *)*text;

    if (*s == 0U) {
        return 0U;
    }

    if ((s[0] & 0x80U) == 0U) {
        *codepoint = s[0];
        *text += 1;
        return 1U;
    }

    if ((s[0] & 0xE0U) == 0xC0U && (s[1] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(s[0] & 0x1FU) << 6U) | (uint32_t)(s[1] & 0x3FU);
        *text += 2;
        return 1U;
    }

    if ((s[0] & 0xF0U) == 0xE0U && (s[1] & 0xC0U) == 0x80U && (s[2] & 0xC0U) == 0x80U) {
        *codepoint = ((uint32_t)(s[0] & 0x0FU) << 12U) |
                     ((uint32_t)(s[1] & 0x3FU) << 6U) |
                     (uint32_t)(s[2] & 0x3FU);
        *text += 3;
        return 1U;
    }

    *codepoint = '?';
    *text += 1;
    return 1U;
}

static int fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t rgb565)
{
    uint16_t linebuf[240];
    struct display_buffer_descriptor desc;
    uint16_t be = sys_cpu_to_be16(rgb565);

    if (!g_ready || w == 0U || h == 0U) {
        return -1;
    }

    if (w > sizeof(linebuf) / sizeof(linebuf[0])) {
        return -1;
    }

    for (uint16_t i = 0; i < w; ++i) {
        linebuf[i] = be;
    }

    desc.buf_size = (size_t)w * sizeof(uint16_t);
    desc.width = w;
    desc.height = 1U;
    desc.pitch = w;

    for (uint16_t row = 0; row < h; ++row) {
        int ret = display_write(g_display, x, y + row, &desc, linebuf);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int draw_glyph(uint16_t x, uint16_t y, const zpix12_glyph_t *glyph)
{
    uint16_t pixbuf[GLYPH_PX * GLYPH_PX];
    struct display_buffer_descriptor desc;
    uint16_t fg = sys_cpu_to_be16(g_fg);
    uint16_t bg = sys_cpu_to_be16(g_bg);

    if (glyph == NULL) {
        return -1;
    }

    for (uint16_t row = 0; row < GLYPH_PX; ++row) {
        uint16_t bits = glyph->rows[row];

        for (uint16_t col = 0; col < GLYPH_PX; ++col) {
            uint16_t mask = (uint16_t)(1U << (11U - col));
            pixbuf[row * GLYPH_PX + col] = (bits & mask) ? fg : bg;
        }
    }

    desc.buf_size = sizeof(pixbuf);
    desc.width = GLYPH_PX;
    desc.height = GLYPH_PX;
    desc.pitch = GLYPH_PX;

    return display_write(g_display, x, y, &desc, pixbuf);
}

int st7735s_log_display_init(void)
{
#if DT_HAS_CHOSEN(zephyr_display)
    g_display = DEVICE_DT_GET(APP_DISPLAY_NODE);
#else
    LOG_ERR("chosen zephyr,display missing");
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
        (void)fill_rect(0U, 0U, g_caps.x_resolution, g_caps.y_resolution, g_bg);
    }

    LOG_INF("display style: white background, black text");

    LOG_INF("st7735s_log_display init done");

    return 0;
}

void st7735s_log_display_line(const char *text)
{
    const char *p = text;
    uint16_t x = 0U;
    uint16_t y;

    if (!g_ready || text == NULL || g_caps.x_resolution == 0U || g_caps.y_resolution == 0U) {
        return;
    }

    if ((uint32_t)g_cursor_y + GLYPH_PX > g_caps.y_resolution) {
        g_cursor_y = 0U;
    }

    y = g_cursor_y;

    (void)fill_rect(0U, y, g_caps.x_resolution, GLYPH_PX + LINE_SPACING, g_bg);

    while (*p != '\0') {
        uint32_t cp;
        const zpix12_glyph_t *glyph;

        if (!utf8_decode_next(&p, &cp)) {
            break;
        }

        if (cp == '\n') {
            break;
        }

        glyph = find_glyph(cp);
        if (glyph == NULL) {
            glyph = find_glyph('?');
            if (glyph == NULL) {
                continue;
            }
        }

        if ((uint32_t)x + GLYPH_PX > g_caps.x_resolution) {
            break;
        }

        if (draw_glyph(x, y, glyph) != 0) {
            break;
        }

        x = (uint16_t)(x + GLYPH_PX + CHAR_SPACING);
    }

    g_cursor_y = (uint16_t)(g_cursor_y + GLYPH_PX + LINE_SPACING);
}
