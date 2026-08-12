/* Redirect acceptance — see include/ota_url.h for why this is its own
   pure module rather than a branch inside ota.c. */
#include "ota_url.h"

#include <stddef.h>
#include <string.h>

#include "config_validate.h" /* config_is_https_url */

ota_redirect_t ota_url_redirect_check(const char *location, int redirects_done, int max_redirects) {
    if (location == NULL || location[0] == '\0')
        return OTA_REDIRECT_NO_TARGET;

    /* Scheme first. See the header: this refusal names an operator
       mistake, and it is equally true at hop 1 and hop 9. */
    if (!config_is_https_url(location))
        return OTA_REDIRECT_NOT_HTTPS;

    /* strnlen rather than strlen because the question is "does it fit
       OTA_URL_MAX", not "how long is it" — no claim of extra safety is
       intended, and none would be true: config_is_https_url above has
       already walked to the NUL. What matters is that an over-long
       target is REFUSED here rather than copied into a bounded buffer
       and followed in its truncated form. */
    if (strnlen(location, OTA_URL_MAX) >= OTA_URL_MAX)
        return OTA_REDIRECT_TOO_LONG;

    /* A negative count is a corrupt caller, and the safe reading of "I do
       not know how many hops I have taken" is "too many". */
    if (redirects_done < 0 || redirects_done >= max_redirects)
        return OTA_REDIRECT_TOO_MANY;

    return OTA_REDIRECT_FOLLOW;
}

const char *ota_url_redirect_str(ota_redirect_t r) {
    switch (r) {
        case OTA_REDIRECT_FOLLOW:
            return "";
        case OTA_REDIRECT_NO_TARGET:
            return "no_target";
        case OTA_REDIRECT_NOT_HTTPS:
            return "not_https";
        case OTA_REDIRECT_TOO_LONG:
            return "too_long";
        case OTA_REDIRECT_TOO_MANY:
            return "too_many";
        default:
            break;
    }
    return "";
}
