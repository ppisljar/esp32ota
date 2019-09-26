#ifndef ESP_PLUGIN_c003_H
#define ESP_PLUGIN_c003_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event_loop.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>



class WiFiPlugin {
    
    public:
        wifi_config_t wifi_config = {};
        bool init();
        
        
};

#endif