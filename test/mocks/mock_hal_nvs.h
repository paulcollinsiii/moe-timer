#pragma once
#include "hal_nvs.h"

/* Reset all stored key-value pairs (and clear failure injection). Call in
   setUp() before each test. */
void mock_nvs_reset(void);
/* Failure injection: the next `count` writes return ESP_FAIL without
   touching the store (-1 = all writes fail until reset). */
void mock_nvs_fail_writes(int count);
