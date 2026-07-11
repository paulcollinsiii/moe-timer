#include "device_id.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "nvs_config.h"

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
    /* Through the config module like every other reader of this key
       (empty-default semantics: unset reads as ""). */
    if (nvs_config_get_dev_name(buf, len) != ESP_OK || buf[0] == '\0') {
        snprintf(buf, len, "%s", device_id());
    }
}
