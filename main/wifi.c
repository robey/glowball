#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "wifi.h"

#define WIFI_MAX_RETRIES 5

enum WifiState {
    OFF = 0,
    CONNECTING,
    WAITING_FOR_IP,
    ONLINE,
} s_state = OFF;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static int retries = 0;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            s_state = CONNECTING;
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            printf("connected!\n");
            s_state = WAITING_FOR_IP;
            retries = 0;
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            printf("disconnected :(\n");
            if (retries < WIFI_MAX_RETRIES) {
                retries++;
                esp_wifi_connect();
            } else {
                printf("giving up on wifi.\n");
                s_state = OFF;
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            // we have an IP address!
            s_state = ONLINE;
        }
    }
}

static void wifi_login(nvs_handle_t nvs_handle) {
    char wifi_ssid[64], wifi_pass[64];
    size_t wifi_ssid_len = sizeof(wifi_ssid), wifi_pass_len = sizeof(wifi_pass);
    strcpy(wifi_ssid, "none");
    strcpy(wifi_pass, "none");
    nvs_get_str(nvs_handle, "wifi-ssid", wifi_ssid, &wifi_ssid_len);
    nvs_get_str(nvs_handle, "wifi-pass", wifi_pass, &wifi_pass_len);
    printf("wifi auth: ssid=%s pass=%s\n", wifi_ssid, wifi_pass);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    strcpy((char *)wifi_config.sta.ssid, wifi_ssid);
    strcpy((char *)wifi_config.sta.password, wifi_pass);
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
}

void wifi_init(nvs_handle_t nvs_handle) {
    // first, start up lwip (netif), the event task, and wifi
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_login(nvs_handle);
    ESP_ERROR_CHECK(esp_wifi_start());
}
