#ifndef OTA_WIFI_H
#define OTA_WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

/* SoftAP fallback parameters — mirror freeesp32_ave so the same recovery
 * network is presented whether the device is running the main app or the
 * OTA updater. */
#define WIFI_AP_SSID     "ESP32-AVE-Setup"
#define WIFI_AP_PASSWORD "entrain123"   /* WPA2, >= 8 chars */
#define WIFI_AP_IP_STR   "192.168.4.1"

/* NVS namespace + keys the main app populates before rebooting into OTA mode.
 * Plain strings (NOT the device's versioned credential blob). */
#define OTA_NVS_NAMESPACE "ota"
#define OTA_NVS_KEY_SSID  "ssid"
#define OTA_NVS_KEY_PASS  "pass"

/*
 * Bring up networking. Reads STA credentials from NVS namespace "ota"
 * (keys "ssid"/"pass"). If present, tries to join that network; on success
 * stays in STA mode. If no creds or the join fails, falls back to a SoftAP
 * (SSID ESP32-AVE-Setup, IP 192.168.4.1) so the updater is always reachable.
 * Blocks until either STA is connected or the SoftAP is up.
 */
void wifi_start(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_WIFI_H */
