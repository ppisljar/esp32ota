#include <sys/param.h>
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"

static const char *TAG = "MAIN";

#include "plugins/c003_wifi.h"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192
#define TAG "ESP32-minimal-ota"

WiFiPlugin *wifi_plugin;
httpd_handle_t server = NULL;

struct file_server_data {
    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

static struct file_server_data *server_data = NULL;

void http_quick_register(const char * uri, httpd_method_t method,  esp_err_t handler(httpd_req_t *req), void *ctx) {
    ESP_LOGI(TAG, "registering handler %p for uri %s", handler, uri);
    httpd_uri_t uri_handler = {
        .uri       = uri,
        .method    = method,
        .handler   = handler,
        .user_ctx  = ctx
    };
    httpd_register_uri_handler(server, &uri_handler);
}

static esp_err_t reboot_handler(httpd_req_t *req) {
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_set_boot_partition(update_partition);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();
}

#define MAX_FLASH_SIZE  2024000
static esp_err_t flash_post_handler(httpd_req_t *req) {

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    if (configured != running) {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGD(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);


    if (req->content_len > MAX_FLASH_SIZE) {
        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
        httpd_resp_set_status(req, "400 Bad Request");
        return ESP_FAIL;
    }

    update_partition = esp_ota_get_next_update_partition(NULL);


    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);
    assert(update_partition != NULL);    

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *buf = ((struct file_server_data *)req->user_ctx)->scratch;
    int received;

    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;
    bool image_header_was_checked = false;
    esp_err_t err;

    while (remaining > 0) {

        ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            ESP_LOGE(TAG, "File reception failed!");
            /* Return failure reason with status code */
            httpd_resp_set_status(req, "500 Server Error");
            httpd_resp_sendstr(req, "Failed to receive file!");
            return ESP_FAIL;
        }

        if (image_header_was_checked == false) {
            esp_app_desc_t new_app_info;
            if (received > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                // check current version with downloading
                memcpy(&new_app_info, &buf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                esp_app_desc_t running_app_info;
                if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                    ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
                }

                const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                esp_app_desc_t invalid_app_info;
                if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                    ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                }

                // check current version with last invalid partition
                if (last_invalid_app != NULL) {
                    if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                        ESP_LOGW(TAG, "New version is the same as invalid version.");
                        ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
                        ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
                        httpd_resp_set_status(req, "500 Server Error");
                        httpd_resp_sendstr(req, "Failed to receive file!");
                        return ESP_FAIL;
                    }
                }

                if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0) {
                    // ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
                    // httpd_resp_set_status(req, "500 Server Error");
                    // httpd_resp_sendstr(req, "Failed to receive file!");
                    // return ESP_FAIL;
                }

                image_header_was_checked = true;

                err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                    httpd_resp_set_status(req, "500 Server Error");
                    httpd_resp_sendstr(req, "Failed to receive file!");
                    return ESP_FAIL;
                }
                ESP_LOGD(TAG, "esp_ota_begin succeeded");
            } else {
                ESP_LOGE(TAG, "received package is not fit len");
                httpd_resp_set_status(req, "500 Server Error");
                httpd_resp_sendstr(req, "Failed to receive file!");
                return ESP_FAIL;
            }
        }
        err = esp_ota_write( update_handle, (const void *)buf, received);
        if (err != ESP_OK) {
            httpd_resp_set_status(req, "500 Server Error");
            httpd_resp_sendstr(req, "Failed to receive file!");
            return ESP_FAIL;
        }
        
       // ESP_LOGD(TAG, "Written image length %d", binary_file_length);

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }

    ESP_LOGI(TAG, "File reception complete");

    /* Redirect onto root to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "File uploaded successfully");

    esp_ota_set_boot_partition(update_partition);

    vTaskDelay(3000 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

extern "C" void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    tcpip_adapter_init();

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    server_data = (file_server_data*)calloc(1, sizeof(struct file_server_data));
    if (!server_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        // reboot
        // boots into the OTA app
        esp_ota_set_boot_partition(update_partition);

        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    ESP_LOGD(TAG, "Starting HTTP Server");
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        // reboot
        // boots into the OTA app
        esp_ota_set_boot_partition(update_partition);

        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    }

    http_quick_register("/update", HTTP_POST, flash_post_handler, server_data);
    http_quick_register("/reboot", HTTP_GET, reboot_handler, server_data);

    wifi_plugin = new WiFiPlugin();
    wifi_plugin->init();

    for (;;) {
        vTaskDelay( 1000 / portTICK_PERIOD_MS);
    }
}

// when configuration is updated: (how to detect it ?) easiest way is to reboot ... web endpoint to save config (thru Config object), somewhere along the way we check what needs to be updated
// - reload config object ? 
// - if wifi config updated, reconnect (call initialize_wifi again)
// - stop all services and restart them
