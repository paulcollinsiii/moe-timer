#pragma once
#ifndef NATIVE
#include "esp_err.h"
#else
#ifndef ESP_COMPAT_ERR_DEFINED
#define ESP_COMPAT_ERR_DEFINED
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#ifndef ESP_ERR_INVALID_VERSION
#define ESP_ERR_INVALID_VERSION 0x10A
#endif
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
#endif
#endif
