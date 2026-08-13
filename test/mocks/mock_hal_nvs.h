#pragma once
#include "hal_nvs.h"

/* Reset all stored key-value pairs (and clear failure injection). Call in
   setUp() before each test. */
void mock_nvs_reset(void);
/* Failure injection: the next `count` writes return ESP_FAIL without
   touching the store (-1 = all writes fail until reset). */
void mock_nvs_fail_writes(int count);
/* Number of read calls (u16/u32/str/blob, hit or miss) for `key` since the
   last mock_nvs_reset() — lets tests assert caching behavior. */
int mock_nvs_read_count(const char *key);
/* Number of write calls (u16/u32/str/blob) for `key` since the last
   mock_nvs_reset(). Counts attempts, including ones turned into ESP_FAIL
   by mock_nvs_fail_writes() and ones that store a byte-identical value:
   the quantity being asserted is flash traffic, which is what wears the
   part out, not whether the stored value ended up different. */
int mock_nvs_write_count(const char *key);
