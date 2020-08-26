#pragma once

#include "driver/rmt.h"

void ws2812b_init(rmt_channel_t channel, int pin);
void ws2812b_test(void);
void ws2812b_set(uint32_t rgb, int count);
