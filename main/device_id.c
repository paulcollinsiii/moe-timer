#include "device_id.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "hal_nvs.h"

const char *device_id(void) {
    static char s_id[16];
    if (s_id[0] == '\0') {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_id, sizeof(s_id), "magtag-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return s_id;
}

void device_name(char *buf, size_t len) {
    size_t rlen = len;
    if (hal_nvs_read_str("dev_name", buf, &rlen) != ESP_OK || buf[0] == '\0') {
        snprintf(buf, len, "%s", device_id());
    }
}
