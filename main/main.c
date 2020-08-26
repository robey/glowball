#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "driver/gpio.h"

#include "cli.h"
#include "http_server.h"
#include "wifi.h"
#include "ws2812b.h"

nvs_handle_t s_nvs_handle;

#define THING_GPIO_LED 5
#define NEOPIXEL_GPIO 13


static void cmd_mem(const void *command_arg, int argc, const char * const *argv) {
    multi_heap_info_t heap_info;
    heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);

    uint32_t total = heap_info.total_free_bytes + heap_info.total_allocated_bytes;

    printf("heap: %u/%u free, low %u, largest %u\n",
        heap_info.total_free_bytes, total, xPortGetMinimumEverFreeHeapSize(), heap_info.largest_free_block);
}

static void cmd_ps(const void *command_arg, int argc, const char * const *argv) {
#define MAX_TASKS 32
    TaskStatus_t tasks[MAX_TASKS];
    uint32_t total_run_time;
    int task_count = uxTaskGetSystemState(tasks, MAX_TASKS, &total_run_time);

    printf("uptime: %d sec\n", total_run_time / 1000000);
    printf("\x1b[4m id name         state pri  run xs-stack\x1b[0m\n");
    for (int i = 0; i < task_count; i++) {
        TaskStatus_t *task = &tasks[i];
        char state = 'X';
        if (task->eCurrentState == eRunning || task->eCurrentState == eReady) {
            state = 'R';
        } else if (task->eCurrentState == eBlocked) {
            state = 'B';
        } else if (task->eCurrentState == eSuspended) {
            state = 'S';
        }
        uint32_t percent = task->ulRunTimeCounter / (total_run_time / 100);
        uint32_t xs = sizeof(portSTACK_TYPE) * task->usStackHighWaterMark;
        printf("%3d %-16s %c %3d %3d%% %7d\n", task->xTaskNumber, task->pcTaskName, state, task->uxCurrentPriority, percent, xs);
    }
}

static void cmd_wifi(const void *command_arg, int argc, const char * const *argv) {
    if (argc < 3) {
        printf("usage: wifi <ssid> <pass>\n");
        return;
    }

    nvs_set_str(s_nvs_handle, "wifi-ssid", argv[1]);
    nvs_set_str(s_nvs_handle, "wifi-pass", argv[2]);
    nvs_commit(s_nvs_handle);
    printf("changed wifi\n");
    // don't bother trying to rebuild wifi, just reboot. it's too complex.
    esp_restart();
}

static void cmd_name(const void *command_arg, int argc, const char * const *argv) {
    if (argc < 2) {
        printf("usage: name <name>\n");
        return;
    }

    nvs_set_str(s_nvs_handle, "name", argv[1]);
    nvs_commit(s_nvs_handle);
    printf("changed name\n");
    esp_restart();
}

static void cmd_config(const void *command_arg, int argc, const char * const *argv) {
    char buffer[64];
    size_t len;
    len = sizeof(buffer);
    nvs_get_str(s_nvs_handle, "name", buffer, &len);
    printf("name: %s\n", buffer);
    len = sizeof(buffer);
    nvs_get_str(s_nvs_handle, "wifi-ssid", buffer, &len);
    printf("ssid: %s\n", buffer);
    len = sizeof(buffer);
    nvs_get_str(s_nvs_handle, "wifi-pass", buffer, &len);
    printf("pass: %s\n", buffer);
}

static void cmd_reboot(const void *command_arg, int argc, const char * const *argv) {
    esp_restart();
}

static void cmd_led(const void *command_arg, int argc, const char * const *argv) {
    gpio_set_level(THING_GPIO_LED, cli_is_truthy(argv[1]));

    ws2812b_test();
}

static cli_command_t commands[] = {
    { "ps", "show task list", cmd_ps, NULL, NULL },
    { "mem", "memory stats", cmd_mem, NULL, NULL },
    { "wifi <ssid> <pass>", "set wifi auth", cmd_wifi, NULL, NULL },
    { "name <name>", "set mdns name", cmd_name, NULL, NULL },
    { "config", "show name & wifi config", cmd_config, NULL, NULL },
    { "reboot", "reboot", cmd_reboot, NULL, NULL },
    { "led", "<on|off>", cmd_led, NULL, NULL },
    CLI_LAST_COMMAND
};


nvs_handle_t flash_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("glowball", NVS_READWRITE, &handle));
    return handle;
}

void app_main(void) {
    printf("Hello robey!\n");

    // gpio #5 is an annoying blue LED
    gpio_config_t gpio_led_conf = {
        .pin_bit_mask = (1 << THING_GPIO_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLDOWN_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_led_conf);
    gpio_set_level(THING_GPIO_LED, 0);

    s_nvs_handle = flash_init();
    wifi_init(s_nvs_handle);

    // start mDNS
    char name[64];
    size_t len = sizeof(name);
    strcpy(name, "default-name");
    nvs_get_str(s_nvs_handle, "name", name, &len);
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(name);

    cli_init(UART_NUM_0, commands);
    http_server_start();
    ws2812b_init(RMT_CHANNEL_0, NEOPIXEL_GPIO);
}
