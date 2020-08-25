#include <stdio.h>
#include "http_server.h"
#include "ws2812b.h"

static uint32_t hex_to_color(const char *hex) {
    uint32_t rv = 0;
    for (int i = 0; i < 6; i++) {
        rv = (rv << 4) | ((hex[i] % 32 + 9) % 25);
    }
    return rv;
}

static void color_to_hex(uint32_t color, char *hex) {
    for (int i = 0; i < 6; i++) {
        uint8_t nybble = (color & (0xf00000 >> (i * 4))) >> ((5 - i) * 4);
        *hex++ = nybble + (nybble / 10) * 39 + 48;
    }
    *hex = 0;
}

static void bad_request(httpd_req_t *req, const char *reason) {
    httpd_resp_set_status(req, "400 Bad request");
    httpd_resp_send(req, reason, HTTPD_RESP_USE_STRLEN);
}

// GET /set?color=RRGGBB
static esp_err_t set_handler(httpd_req_t *req) {
    char query[40];
    esp_err_t err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (err != ESP_OK) {
        bad_request(req, "query string too long or fucked up");
        return ESP_OK;
    }

    char hex[8];
    err = httpd_query_key_value(query, "color", hex, sizeof(hex));
    if (err != ESP_OK) {
        bad_request(req, "missing or fucked up 'color' param");
        return ESP_OK;
    }

    if (strlen(hex) != 6) {
        bad_request(req, "color must be exactly 6 chars");
        return ESP_OK;
    }

    int count = 16;
    char count_str[8];
    if (httpd_query_key_value(query, "count", count_str, sizeof(count_str)) == ESP_OK) {
        count = atoi(count_str);
    }

    ws2812b_set(hex_to_color(hex), count);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static httpd_uri_t get_set_uri = {
    .uri      = "/set",
    .method   = HTTP_GET,
    .handler  = set_handler,
    .user_ctx = NULL,
};

static httpd_uri_t post_set_uri = {
    .uri      = "/set",
    .method   = HTTP_POST,
    .handler  = set_handler,
    .user_ctx = NULL,
};


static esp_err_t handler(httpd_req_t *req) {
    /* Send a simple response */
    const char resp[] = "hello from the glowball!";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_uri_t uri_get = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = handler,
    .user_ctx = NULL
};

httpd_handle_t http_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_register_uri_handler(server, &uri_get);
    httpd_register_uri_handler(server, &get_set_uri);
    httpd_register_uri_handler(server, &post_set_uri);

    return server;
}
