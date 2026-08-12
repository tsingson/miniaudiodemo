#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
    printk("hello copilot\r\n");

    while (1) {
        k_sleep(K_SECONDS(1));
    }

    return 0;
}
