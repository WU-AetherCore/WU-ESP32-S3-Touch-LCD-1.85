#include "Wireless.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "ST77916.h"
#include "QMI8658.h"
#include "BAT_Driver.h"
#include "SD_MMC.h"
#include "PCF85063.h"
#include "esp_heap_caps.h"

uint16_t WIFI_NUM = 0;
bool Scan_finish = false;
bool WiFi_Connected = false;
char WiFi_IP[16] = "0.0.0.0";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/* ============ HTTP handlers ============ */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    char buf[2048];
    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32-S3</title>"
        "<style>"
        "body{font-family:sans-serif;background:#111;color:#eee;padding:20px}"
        "h1{color:#7B68EE;text-align:center}"
        ".card{background:#1a1a2e;border-radius:10px;padding:14px;margin:10px 0}"
        ".row{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid #333}"
        ".lbl{color:#888} .val{font-weight:bold}"
        ".dot{display:inline-block;width:10px;height:10px;border-radius:50%%;margin-right:6px}"
        ".green{background:#00E676} .red{background:#FF4757}"
        "</style></head><body>"
        "<h1>ESP32-S3 Device</h1>"
        "<div class='card'>"
        "<div class='row'><span class='lbl'>WiFi</span><span class='val'><span class='dot %s'></span>%s</span></div>"
        "<div class='row'><span class='lbl'>IP</span><span class='val'>%s</span></div></div>"
        "<div class='card'>"
        "<div class='row'><span class='lbl'>PSRAM Total</span><span class='val'>%lu KB</span></div>"
        "<div class='row'><span class='lbl'>PSRAM Free</span><span class='val'>%lu KB</span></div>"
        "<div class='row'><span class='lbl'>Heap Free</span><span class='val'>%lu KB</span></div></div>"
        "<div class='card'>"
        "<div class='row'><span class='lbl'>Battery</span><span class='val'>%.2f V</span></div>"
        "<div class='row'><span class='lbl'>Accel</span><span class='val'>X:%.1f Y:%.1f Z:%.1f</span></div></div>"
        "<div class='card'>"
        "<div class='row'><span class='lbl'>Time</span><span class='val'>%02d:%02d:%02d</span></div></div>"
        "</body></html>",
        WiFi_Connected ? "green" : "red",
        WiFi_Connected ? "Connected" : "Disconnected",
        WiFi_IP,
        (unsigned long)(ps_total / 1024), (unsigned long)(ps_free / 1024),
        (unsigned long)(heap_free / 1024),
        BAT_analogVolts,
        (double)Accel.x, (double)Accel.y, (double)Accel.z,
        datetime.hour, datetime.minute, datetime.second);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, buf, strlen(buf));
}

static const httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };

/* ============ Wi-Fi event handler ============ */
static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED:
            WiFi_Connected = false;
            if (s_retry_num < 10) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGW("WIFI", "Disconnected, retrying (%d)...", s_retry_num);
            }
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(WiFi_IP, sizeof(WiFi_IP), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        WiFi_Connected = true;
        xEventGroupSetBits(s_wifi_event_group, BIT0);
        ESP_LOGI("WIFI", "Got IP: %s", WiFi_IP);
    }
}

/* ============ Wi-Fi task ============ */
static void wifi_task(void *arg)
{
    (void)arg;

    /* --- network stack --- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    s_wifi_event_group = xEventGroupCreate();

    /* --- event handlers --- */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL, NULL));

    /* --- Wi-Fi init + config --- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wcfg = { .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    strncpy((char *)wcfg.sta.ssid,     "WU",         sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password,  "WU12345678", sizeof(wcfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI("WIFI", "Connecting to SSID:WU ...");

    /* wait up to 30 s for an IP */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, BIT0, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & BIT0) {
        ESP_LOGI("WIFI", "Connected – IP %s", WiFi_IP);
        Scan_finish = true;

        /* start tiny web server */
        httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
        hc.stack_size = 8192;
        httpd_handle_t srv = NULL;
        if (httpd_start(&srv, &hc) == ESP_OK) {
            httpd_register_uri_handler(srv, &uri_root);
            ESP_LOGI("WEB", "Server running at http://%s/", WiFi_IP);
        }
    } else {
        ESP_LOGW("WIFI", "Timed out waiting for IP");
        Scan_finish = true;
    }

    vTaskDelete(NULL);
}

void Wireless_Init(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    xTaskCreatePinnedToCore(wifi_task, "wifi", 8192, NULL, 1, NULL, 0);
    ESP_LOGI("WIFI", "WiFi task launched");
}
