#include <zephyr/init.h>
#include <zephyr/irq.h>

#include <stm32f4xx_ll_bus.h>
#include <stm32f4xx_ll_gpio.h>

#include "device/dcd.h"

static void tinyusb_otg_fs_isr(void *argument)
{
    (void)argument;
    dcd_int_handler(0);
}

static int tinyusb_otg_fs_init(void)
{
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_11, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_12, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_8_15(GPIOA, LL_GPIO_PIN_11, LL_GPIO_AF_10);
    LL_GPIO_SetAFPin_8_15(GPIOA, LL_GPIO_PIN_12, LL_GPIO_AF_10);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_11, LL_GPIO_SPEED_FREQ_VERY_HIGH);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_12, LL_GPIO_SPEED_FREQ_VERY_HIGH);
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_OTGFS);
    IRQ_CONNECT(OTG_FS_IRQn, 1, tinyusb_otg_fs_isr, NULL, 0);
    return 0;
}

SYS_INIT(tinyusb_otg_fs_init, PRE_KERNEL_1, 0);