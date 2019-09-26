#include "c003_wifi.h"
#include "mdns.h"


static const char *TAG = "WiFiPlugin";

static esp_err_t ap_mode(WiFiPlugin &p) {
    esp_wifi_stop();
    p.wifi_config = {};
    strcpy((char*)p.wifi_config.ap.ssid, "ESP32Ctrl");
    strcpy((char*)p.wifi_config.ap.password, "");
    p.wifi_config.ap.ssid_len = strlen((char*)p.wifi_config.ap.ssid);
    p.wifi_config.ap.max_connection = 5;
    p.wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &p.wifi_config));
    esp_wifi_start();

    return ESP_OK;
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
        case SYSTEM_EVENT_STA_START: {
            wifi_mode_t mode;
            esp_wifi_get_mode(&mode);
            ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START");
            if (mode == WIFI_MODE_STA)
                ESP_ERROR_CHECK(esp_wifi_connect());   
            break;
        }
        case SYSTEM_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
            ESP_LOGI(TAG, "Got IP: '%s'", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "SYSTEM_EVENT_STA_DISCONNECTED");
			ESP_LOGI(TAG, "reason: %d\n",event->event_info.disconnected.reason);
            ESP_ERROR_CHECK(esp_wifi_connect());
            break;
        default:
            ESP_LOGI(TAG, "SYSTEM_EVENT_WifI_%d", event->event_id);
            break;
    }
    return ESP_OK;
}

bool WiFiPlugin::init() {

    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, this));

    wifi_init_config_t cfgw = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfgw));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));


    if (ESP_OK == esp_wifi_get_config(WIFI_IF_STA, &wifi_config) && wifi_config.sta.ssid[0] != 0) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_LOGI(TAG, "Connect to stored Wi-Fi SSID:%s", wifi_config.sta.ssid);
    } 
    else {
        ESP_LOGW(TAG, "No wifi SSID stored!");
        // reboot
    }
    
    ESP_ERROR_CHECK(esp_wifi_start());

    return true;
}

