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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc.h"
#include "xtensa/hal.h"

#include "ws2812b.h"

#define LONG_PULSE_NS 850
#define SHORT_PULSE_NS 400
#define RESET_NS 50000

#define MILLION (1000 * 1000)

rmt_channel_t s_rmt_channel;
rmt_item32_t s_rmt_bit_0;
rmt_item32_t s_rmt_bit_1;
rmt_item32_t s_rmt_reset;


void ws2812b_init(rmt_channel_t channel, int pin) {
    s_rmt_channel = channel;

    /*
     * APB clock is normally 80MHz (12.5 ns). all our timings are multiples
     * of 50 ns, so ideally we can let the RMT driver "relax" with a clock
     * divide of 4, to get 50 ns ticks.
     */
    uint32_t apb_freq_mhz = rtc_clk_apb_freq_get() / MILLION;
    uint32_t divide = apb_freq_mhz == 80 ? 4 : (apb_freq_mhz == 40 ? 2 : 1);

    double tick_ns = 1000.0 / ((double) apb_freq_mhz / divide);
    uint32_t long_cycles = (uint32_t) ((double) LONG_PULSE_NS / tick_ns);
    uint32_t short_cycles = (uint32_t) ((double) SHORT_PULSE_NS / tick_ns);
    uint32_t reset_cycles = (uint32_t) ((double) RESET_NS / tick_ns);

    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(pin, channel);
    config.clk_div = divide;
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(channel, 0, 0));

    // precompute the on/off cycle for a 0 or 1 bit
    s_rmt_bit_0.duration0 = short_cycles;
    s_rmt_bit_0.level0 = 1;
    s_rmt_bit_0.duration1 = long_cycles;
    s_rmt_bit_0.level1 = 0;

    s_rmt_bit_1.duration0 = long_cycles;
    s_rmt_bit_1.level0 = 1;
    s_rmt_bit_1.duration1 = short_cycles;
    s_rmt_bit_1.level1 = 0;

    s_rmt_reset.duration0 = reset_cycles;
    s_rmt_reset.level0 = 0;
    s_rmt_reset.duration1 = 1;
    s_rmt_reset.level1 = 0;

    printf("ws2812b_init: tick=%f, long=%d, short=%d, reset=%d\n", tick_ns, long_cycles, short_cycles, reset_cycles);
}

static void rmt_transmit(const uint8_t *data, size_t len) {
    size_t rmt_count = 8 * len + 1;
    rmt_item32_t *buffer = malloc(rmt_count * sizeof(rmt_item32_t));
    if (buffer == NULL) {
        printf("ERROR: failed to malloc rmt\n");
        return;
    }

    rmt_item32_t *p = buffer;
    while (len > 0) {
        for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
            p->val = (((*data & mask) != 0) ? s_rmt_bit_1 : s_rmt_bit_0).val;
            p++;
        }
        data++, len--;
    }
    p->val = s_rmt_reset.val;

    // waits until done
    ESP_ERROR_CHECK(rmt_write_items(s_rmt_channel, buffer, rmt_count, 1));
    free(buffer);
}

void ws2812b_set(uint32_t rgb, int count) {
    printf("ws2812b_set: %06x, count %d\n", rgb, count);
    uint8_t *message = malloc(3 * count);
    for (int i = 0; i < count; i++) {
        // convert to GRB
        message[i * 3] = (rgb >> 8) & 0xff;
        message[i * 3 + 1] = (rgb >> 16) & 0xff;
        message[i * 3 + 2] = rgb & 0xff;
    }
    rmt_transmit(message, 3 * count);
    free(message);
}
