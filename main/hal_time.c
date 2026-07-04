#include "hal_time.h"

#include <time.h>

time_t hal_time_now(void) {
    return time(NULL);
}
