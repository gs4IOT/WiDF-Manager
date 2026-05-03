/*  custom_config — WIDF Manager custom configuration example
 *
 *  Demonstrates runtime configuration using widf_mngr_config_t
 *  and flow control modes.
 *  Override only the fields you need — the rest use Kconfig defaults.
 */

#include <string.h>
#include "widf_mngr.h"

void app_main(void)
{
    widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();

    /* Custom AP name and mDNS hostname */
    strncpy(cfg.ap_ssid,  "MyDevice-Setup", sizeof(cfg.ap_ssid));
    strncpy(cfg.hostname, "mydevice",        sizeof(cfg.hostname));

    /* AP security — uncomment to enable WPA2/WPA3 with a custom password.
     * Minimum 8 characters required. If the default SSID is used, the last
     * 4 MAC hex chars are appended automatically (e.g. WIDF-MANAGER-A348).
     * A MAC-derived fallback password is used if the password is invalid. */
    // strncpy(cfg.ap_password, "mypassword", sizeof(cfg.ap_password));
    // cfg.ap_authmode = WIFI_AUTH_WPA2_WPA3_PSK;

    /* Portal authentication — uncomment to protect /ota and /erase.
     * Leave empty (default) to disable authentication entirely. */
    // strncpy(cfg.auth_password, "adminpass", sizeof(cfg.auth_password));

    /* GPIO long press to reopen portal — adjust for your board:
     *   GPIO3  — M5Stamp C3 (default)
     *   GPIO0  — ESP32 DevKit
     *   GPIO9  — ESP32-C6 / ESP32-H2 DevKit */
    cfg.reopen_gpio   = 9;
    cfg.long_press_ms = 2000;

    /* Portal closes after 60 seconds of inactivity */
    cfg.portal_timeout_s = 60;

    /* Flow control — what happens when no saved network connects on boot:
     *   WIDF_FALLBACK_AP_PORTAL  — open AP and portal (default)
     *   WIDF_FALLBACK_EVENT_ONLY — fire event and return to host */
    cfg.fallback_mode = WIDF_FALLBACK_AP_PORTAL;

    /* Flow control — what happens after credentials are saved:
     *   WIDF_ON_SAVE_RECONNECT   — reconnect without rebooting (default)
     *   WIDF_ON_SAVE_EVENT_ONLY  — fire event, host decides */
    cfg.on_save_mode = WIDF_ON_SAVE_RECONNECT;

    /* Flow control — what happens after STA connects successfully:
     *   WIDF_ON_CONNECT_AP_DOWN    — bring AP down (default)
     *   WIDF_ON_CONNECT_PORTAL_STA — portal stays up on STA IP
     *   WIDF_ON_CONNECT_EVENT_ONLY — fire event, host decides */
    cfg.on_connect_mode = WIDF_ON_CONNECT_PORTAL_STA;

    widf_mngr_run(&cfg);
}
