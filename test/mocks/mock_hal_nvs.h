#pragma once
#include "hal_nvs.h"

/* Reset all stored key-value pairs. Call in setUp() before each test. */
void mock_nvs_reset(void);
