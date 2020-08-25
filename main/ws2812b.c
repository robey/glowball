/*
 * ws2812b (aka neopixel) protocol:
 *   - 0 bit: 400ns high, 850ns low
 *   - 1 bit: 850ns high, 400ns low
 *   - reset: 50_000ns low (40 bits of all-low)
 *
 * all timings +/- 150ns. 1250ns per bit = 800Kb/s.
 *
 * each led gets 24 bits, in GRB format, high bit first. it absorbs the first
 * 24 bits it sees, then passes on all the rest until it sees a reset.
 */

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc.h"
#include "xtensa/hal.h"

#define LONG_PULSE_NS 850
#define SHORT_PULSE_NS 400
#define RESET_NS 50000

uint32_t s_long_cycles = 0;
uint32_t s_short_cycles = 0;
uint32_t s_reset_cycles = 0;
int s_pin = 0;

void ws2812b_init(int pin) {
    rtc_cpu_freq_config_t freq_config;
    rtc_clk_cpu_freq_get_config(&freq_config);
    // convert long & short (ns) into clock cycles
    s_long_cycles = freq_config.freq_mhz * LONG_PULSE_NS / 1000;
    s_short_cycles = freq_config.freq_mhz * SHORT_PULSE_NS / 1000;
    s_reset_cycles = freq_config.freq_mhz * RESET_NS / 1000;
    s_pin = pin;

    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1 << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLDOWN_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_conf);
}

static void rmt_init(rmt_channel_t channel, int pin) {
    // APB clock is normally 80MHz
    uint32_t apb_freq = rtc_clk_apb_freq_get();
#define CLOCK_DIVIDE 1234
    rmt_config_t config = {
        .rmt_mode = RMT_MODE_TX,
        .channel = channel,
        .gpio_num = pin,
        .clk_div = CLOCK_DIVIDE,
        .mem_block_num = 1,
        .tx_config = {
            .loop_en = false,
            .carrier_en = false,
            .idle_output_en = true,
            .idle_level = 0,
        },
    };
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(channel, 0, 0));
}

// bit banging because i am an awful, horrible person.
static void transmit(const uint8_t *data, size_t len) {
    taskDISABLE_INTERRUPTS();

    gpio_set_level(s_pin, 0);
    uint32_t start = xthal_get_ccount();
    while (xthal_get_ccount() - start < s_long_cycles + s_short_cycles);

    while (len > 0) {
        for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
            uint32_t flip_time = ((*data & mask) != 0) ? s_long_cycles : s_short_cycles;

            start = xthal_get_ccount();
            gpio_set_level(s_pin, 1);
            while (xthal_get_ccount() - start < flip_time);
            gpio_set_level(s_pin, 0);
            while (xthal_get_ccount() - start < s_long_cycles + s_short_cycles);
        }
        data++, len--;
    }

    // stall for 50us
    start = xthal_get_ccount();
    while (xthal_get_ccount() - start < s_reset_cycles);

    taskENABLE_INTERRUPTS();
}

void ws2812b_set(uint32_t rgb, int count) {
    printf("set: %06x, count %d\n", rgb, count);
    uint8_t *message = malloc(3 * count);
    for (int i = 0; i < count; i++) {
        // convert to GRB
        message[i * 3] = (rgb >> 8) & 0xff;
        message[i * 3 + 1] = (rgb >> 16) & 0xff;
        message[i * 3 + 2] = rgb & 0xff;
    }
    transmit(message, 3 * count);
    free(message);
}

void ws2812b_test(void) {
    const uint8_t grb[] = {
        0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0,
        0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0,
        0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0,
        0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0, 0x40, 0, 0,
    };
    transmit(grb, 3 * 16);
}
