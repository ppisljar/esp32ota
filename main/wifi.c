#include "wifi.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "ota_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* Initial association attempts before giving up and falling back to SoftAP. */
#define WIFI_MAX_RETRY     6

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            esp_wifi_connect();
            ESP_LOGI(TAG, "STA disconnected, retry (%d)", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Load STA credentials from NVS namespace "ota". Returns true if an SSID
 * was found. ssid/pass buffers must be at least 33/65 bytes. */
static bool load_sta_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace '%s' not found", OTA_NVS_NAMESPACE);
        return false;
    }

    bool ok = false;
    size_t len = ssid_len;
    if (nvs_get_str(h, OTA_NVS_KEY_SSID, ssid, &len) == ESP_OK && ssid[0] != '\0') {
        len = pass_len;
        pass[0] = '\0';
        nvs_get_str(h, OTA_NVS_KEY_PASS, pass, &len); /* password optional */
        ok = true;
    }
    nvs_close(h);
    return ok;
}

static void start_softap(void)
{
    ESP_LOGW(TAG, "Falling back to SoftAP (SSID:%s IP:%s)", WIFI_AP_SSID, WIFI_AP_IP_STR);

    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, WIFI_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    strncpy((char *)ap_config.ap.password, WIFI_AP_PASSWORD, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = (strlen(WIFI_AP_PASSWORD) >= 8) ? WIFI_AUTH_WPA2_PSK
                                                            : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP up — join '%s' (pw '%s') and browse to http://%s",
             WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_IP_STR);
}

void wifi_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!load_sta_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        start_softap();
        return;
    }

    ESP_LOGI(TAG, "Connecting to stored STA SSID: %s", ssid);
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);

    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected");
    } else {
        ESP_LOGW(TAG, "STA connect failed after %d attempts", s_retry_num);
        start_softap();
    }
}
