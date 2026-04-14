/*  custom_config — WIDF Manager custom configuration example
 *
 *  Demonstrates runtime configuration using widf_mngr_config_t.
 *  Override only the fields you need — the rest use Kconfig defaults.
 */

#include <string.h>
#include "widf_mngr.h"

void app_main(void)
{
    widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();

    /* Custom AP name */
    strncpy(cfg.ap_ssid, "MyDevice-Setup", sizeof(cfg.ap_ssid));

    /* Custom mDNS hostname — portal reachable at mydevice.local */
    strncpy(cfg.hostname, "mydevice", sizeof(cfg.hostname));

    /* Portal stays running on STA IP after connect */
    cfg.sta_access = true;

    /* GPIO9 long press to reopen portal (ESP32-C6 DevKit default) */
    cfg.reopen_gpio   = 9;
    cfg.long_press_ms = 2000;

    /* Shorter timeout — close portal after 60 seconds */
    cfg.portal_timeout_s = 60;

    widf_mngr_run(&cfg);
}
