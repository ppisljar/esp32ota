#include <sys/param.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/sha256.h"

#include "wifi.h"

#define TAG "ota"

/* Scratch buffer size for streaming the upload. */
#define SCRATCH_BUFSIZE 4096

/* SHA-256 digest size, in bytes / hex chars. */
#define SHA256_LEN     32
#define SHA256_HEX_LEN 64

static httpd_handle_t server = NULL;
static char *scratch = NULL;

/* ---- helpers ---------------------------------------------------------- */

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL;
}

/* Resolve the target partition for /update from the query string.
 *   ?part=<label>            explicit label
 *   ?type=app|data&subtype=N explicit type/subtype
 *   (none)                   next app OTA partition (default / main-app slot)
 * Returns NULL on a malformed/unknown target (and logs why). */
static const esp_partition_t *resolve_target(httpd_req_t *req)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) {
        return esp_ota_get_next_update_partition(NULL); /* default */
    }

    char *query = malloc(qlen);
    if (!query) return NULL;
    if (httpd_req_get_url_query_str(req, query, qlen) != ESP_OK) {
        free(query);
        return esp_ota_get_next_update_partition(NULL);
    }

    const esp_partition_t *target = NULL;
    char val[32];

    if (httpd_query_key_value(query, "part", val, sizeof(val)) == ESP_OK) {
        target = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
                                          ESP_PARTITION_SUBTYPE_ANY, val);
        if (!target) ESP_LOGE(TAG, "partition label '%s' not found", val);
    } else if (httpd_query_key_value(query, "type", val, sizeof(val)) == ESP_OK) {
        esp_partition_type_t ptype;
        if (strcmp(val, "app") == 0) {
            ptype = ESP_PARTITION_TYPE_APP;
        } else if (strcmp(val, "data") == 0) {
            ptype = ESP_PARTITION_TYPE_DATA;
        } else {
            ESP_LOGE(TAG, "bad type '%s'", val);
            free(query);
            return NULL;
        }
        char sub[16];
        int subtype = ESP_PARTITION_SUBTYPE_ANY;
        if (httpd_query_key_value(query, "subtype", sub, sizeof(sub)) == ESP_OK) {
            subtype = (int)strtol(sub, NULL, 0);
        }
        target = esp_partition_find_first(ptype, subtype, NULL);
        if (!target) ESP_LOGE(TAG, "type=%s subtype=%d not found", val, subtype);
    } else {
        target = esp_ota_get_next_update_partition(NULL);
    }

    free(query);
    return target;
}

/* Decode exactly SHA256_HEX_LEN hex chars from `hex` into `out` (32 bytes).
 * Returns true on success, false if any char is non-hex. */
static bool hex_to_bytes(const char *hex, uint8_t *out)
{
    for (int i = 0; i < SHA256_LEN; i++) {
        int hi = -1, lo = -1;
        char c;
        c = hex[2 * i];
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        c = hex[2 * i + 1];
        if (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* Constant-time comparison of two SHA256_LEN-byte buffers. Returns true if
 * equal. Does not short-circuit, so timing does not leak match position. */
static bool ct_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < SHA256_LEN; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* Result of parsing the expected hash for /update. */
typedef enum {
    HASH_NONE,    /* no sha256 supplied */
    HASH_OK,      /* supplied and decoded into `out` */
    HASH_BAD,     /* supplied but not 64 valid hex chars */
} hash_parse_t;

/* Look for an expected SHA-256, first in the query string (?sha256=<64-hex>),
 * then in the X-Expected-SHA256 request header. On HASH_OK, `out` holds 32
 * decoded bytes. */
static hash_parse_t parse_expected_hash(httpd_req_t *req, uint8_t *out)
{
    char hex[SHA256_HEX_LEN + 1];
    bool found = false;

    /* Query string ?sha256=<hex> */
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1) {
        char *query = malloc(qlen);
        if (query) {
            if (httpd_req_get_url_query_str(req, query, qlen) == ESP_OK &&
                httpd_query_key_value(query, "sha256", hex, sizeof(hex)) == ESP_OK) {
                found = true;
            }
            free(query);
        }
    }

    /* Header X-Expected-SHA256 (only if not in query). */
    if (!found) {
        size_t hlen = httpd_req_get_hdr_value_len(req, "X-Expected-SHA256");
        if (hlen > 0) {
            if (hlen != SHA256_HEX_LEN) return HASH_BAD;
            if (httpd_req_get_hdr_value_str(req, "X-Expected-SHA256",
                                            hex, sizeof(hex)) != ESP_OK) {
                return HASH_BAD;
            }
            found = true;
        }
    }

    if (!found) return HASH_NONE;
    if (strlen(hex) != SHA256_HEX_LEN) return HASH_BAD;
    if (!hex_to_bytes(hex, out)) return HASH_BAD;
    return HASH_OK;
}

/* ---- handlers --------------------------------------------------------- */

static esp_err_t root_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    char msg[96];
    snprintf(msg, sizeof(msg), "OTA updater ready (running: %s @ 0x%08x)\n",
             running ? running->label : "?",
             (unsigned)(running ? running->address : 0));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    /* Boot the app slot (the partition that is NOT the running OTA updater). */
    const esp_partition_t *boot = esp_ota_get_next_update_partition(NULL);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Rebooting into app...\n");
    if (boot) {
        esp_ota_set_boot_partition(boot);
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t update_handler(httpd_req_t *req)
{
    const esp_partition_t *target = resolve_target(req);
    if (!target) {
        return fail(req, "404 Not Found", "Target partition not found");
    }

    bool is_app = (target->type == ESP_PARTITION_TYPE_APP);
    int remaining = req->content_len;

    /* Expected SHA-256 of the upload. Mandatory for app targets (unless
     * relaxed at build time), optional for data targets. */
    uint8_t expected[SHA256_LEN];
    hash_parse_t hp = parse_expected_hash(req, expected);
    bool verify_hash = (hp == HASH_OK);

    if (hp == HASH_BAD) {
        return fail(req, "400 Bad Request",
                    "Malformed sha256 (need 64 hex chars)");
    }
#if CONFIG_OTA_REQUIRE_APP_HASH
    if (is_app && hp == HASH_NONE) {
        return fail(req, "400 Bad Request",
                    "sha256 required for app updates");
    }
#endif

    if (remaining <= 0) {
        return fail(req, "400 Bad Request", "Empty body");
    }
    if ((size_t)remaining > target->size) {
        ESP_LOGE(TAG, "upload %d > partition %s size %u",
                 remaining, target->label, (unsigned)target->size);
        return fail(req, "413 Payload Too Large", "Image larger than target partition");
    }

    ESP_LOGI(TAG, "Flashing %s partition '%s' (0x%08x, %u bytes) with %d bytes",
             is_app ? "app" : "data", target->label,
             (unsigned)target->address, (unsigned)target->size, remaining);

    esp_ota_handle_t ota = 0;
    size_t data_off = 0;
    esp_err_t err;

    if (is_app) {
        err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            return fail(req, "500 Server Error", "esp_ota_begin failed");
        }
    } else {
        /* Data partition: full erase then sequential write. */
        err = esp_partition_erase_range(target, 0, target->size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
            return fail(req, "500 Server Error", "Partition erase failed");
        }
    }

    bool header_logged = false;

    mbedtls_sha256_context sha;
    if (verify_hash) {
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0 /* is224 = 0 -> SHA-256 */);
    }

    while (remaining > 0) {
        int received = httpd_req_recv(req, scratch, MIN(remaining, SCRATCH_BUFSIZE));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (verify_hash) mbedtls_sha256_free(&sha);
            if (is_app) esp_ota_abort(ota);
            return fail(req, "500 Server Error", "Reception failed");
        }

        if (verify_hash) mbedtls_sha256_update(&sha, (const unsigned char *)scratch, received);

        /* Best-effort version sanity logging from the app image header. */
        if (is_app && !header_logged &&
            received > (int)(sizeof(esp_image_header_t) +
                             sizeof(esp_image_segment_header_t) +
                             sizeof(esp_app_desc_t))) {
            esp_app_desc_t new_info;
            memcpy(&new_info,
                   &scratch[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)],
                   sizeof(esp_app_desc_t));
            ESP_LOGI(TAG, "Incoming app version: %s", new_info.version);
            esp_app_desc_t run_info;
            if (esp_ota_get_partition_description(esp_ota_get_running_partition(),
                                                  &run_info) == ESP_OK) {
                ESP_LOGI(TAG, "Running (updater) version: %s", run_info.version);
            }
            header_logged = true; /* do not hard-fail on same version */
        }

        if (is_app) {
            err = esp_ota_write(ota, scratch, received);
        } else {
            err = esp_partition_write(target, data_off, scratch, received);
            data_off += received;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
            if (verify_hash) mbedtls_sha256_free(&sha);
            if (is_app) esp_ota_abort(ota);
            return fail(req, "500 Server Error", "Write failed");
        }
        remaining -= received;
    }

    /* Finalize and verify the content hash before committing anything. */
    if (verify_hash) {
        uint8_t computed[SHA256_LEN];
        mbedtls_sha256_finish(&sha, computed);
        mbedtls_sha256_free(&sha);
        if (!ct_equal(computed, expected)) {
            ESP_LOGE(TAG, "SHA-256 mismatch on '%s' upload", target->label);
            if (is_app) {
                /* Never commit/boot an unverified app image. */
                esp_ota_abort(ota);
            } else {
                /* The freshly written data partition is suspect: re-erase it
                 * so a partial/wrong image is not left behind. */
                esp_err_t e = esp_partition_erase_range(target, 0, target->size);
                if (e != ESP_OK) {
                    ESP_LOGE(TAG, "re-erase after mismatch failed: %s",
                             esp_err_to_name(e));
                }
            }
            return fail(req, "422 Unprocessable Entity",
                        "SHA-256 mismatch; image rejected");
        }
        ESP_LOGI(TAG, "SHA-256 verified for '%s'", target->label);
    }

    if (is_app) {
        err = esp_ota_end(ota);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            return fail(req, "500 Server Error", "esp_ota_end failed (bad image?)");
        }
        err = esp_ota_set_boot_partition(target);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
            return fail(req, "500 Server Error", "set_boot_partition failed");
        }
        ESP_LOGI(TAG, "App written, boot set to '%s'. Rebooting.", target->label);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "App flashed. Rebooting into new firmware.\n");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } else {
        ESP_LOGI(TAG, "Data partition '%s' written (%u bytes).",
                 target->label, (unsigned)data_off);
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Data partition flashed.\n");
    }
    return ESP_OK;
}

static void register_handler(const char *uri, httpd_method_t method,
                             esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t h = { .uri = uri, .method = method, .handler = fn, .user_ctx = NULL };
    httpd_register_uri_handler(server, &h);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    scratch = malloc(SCRATCH_BUFSIZE);
    if (!scratch) {
        ESP_LOGE(TAG, "no memory for scratch buffer");
        esp_restart();
    }

    wifi_start();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        esp_restart();
    }

    register_handler("/", HTTP_GET, root_handler);
    register_handler("/update", HTTP_POST, update_handler);
    register_handler("/reboot", HTTP_GET, reboot_handler);

    ESP_LOGI(TAG, "OTA updater HTTP server started");
}
