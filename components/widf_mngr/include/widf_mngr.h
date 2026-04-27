/*  widf_mngr.h — Public API for WIDF Manager
 *    WIDF Manager v1.2.0 — ESP-IDF native, ESP32 family
 *
 *    Usage:
 *      1. Fill in a widf_mngr_config_t struct
 *      2. Call widf_mngr_run() from your app_main()
 *      3. widf_mngr_run() returns when the portal closes
 *
 *    Example:
 *      widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();
 *      strncpy(cfg.ap_ssid, "MyDevice", sizeof(cfg.ap_ssid));
 *      widf_mngr_run(&cfg);
 */

#ifndef WIDF_MNGR_H
#define WIDF_MNGR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

/* ── Event system ────────────────────────────────────────────────────────── */

typedef enum {
    WIDF_EVENT_STA_TRYING,        /* Attempting connect to a network     */
    WIDF_EVENT_STA_CONNECTED,     /* STA got IP successfully             */
    WIDF_EVENT_STA_DISCONNECTED,  /* STA lost connection mid-session     */
    WIDF_EVENT_STA_FAILED,        /* All retries exhausted for network   */
    WIDF_EVENT_PORTAL_OPENED,     /* Portal HTTP server started          */
    WIDF_EVENT_PORTAL_CLOSED,     /* Portal HTTP server stopped          */
    WIDF_EVENT_CREDENTIALS_SAVED, /* New credentials written to NVS      */
} widf_event_t;

typedef struct {
    widf_event_t event;
    union {
        struct { char ssid[33]; int retries; } trying;       /* per network, before retries begin  */
        struct { char ip[16];   char ssid[33]; } connected;  /* IP and SSID on successful connect  */
        struct { char ssid[33]; } disconnected;              /* SSID of dropped mid-session link   */
        struct { char ssid[33]; int retries; } failed;       /* SSID and retries attempted         */
        struct { char ssid[33]; } saved;                     /* SSID of newly saved credentials    */
    } data;
} widf_event_data_t;

typedef void (*widf_event_cb_t)(const widf_event_data_t *event);

/* ── Flow control enums ──────────────────────────────────────────────────── */

/* What happens when no credentials found or STA connect fails on boot */
typedef enum {
    WIDF_FALLBACK_AP_PORTAL,  /* default — start AP and open portal         */
    WIDF_FALLBACK_EVENT_ONLY, /* fire STA_FAILED event, return to host      */
} widf_fallback_mode_t;

/* What happens after credentials are saved via the portal */
typedef enum {
    WIDF_ON_SAVE_RECONNECT,   /* default — close portal, restart WiFi stack,
    attempt connect with new credentials        */
    WIDF_ON_SAVE_EVENT_ONLY,  /* fire CREDENTIALS_SAVED, host handles it    */
} widf_on_save_mode_t;

/* What happens after STA connects successfully */
typedef enum {
    WIDF_ON_CONNECT_AP_DOWN,     /* default — AP and portal come down       */
    WIDF_ON_CONNECT_PORTAL_STA,  /* portal stays up on STA IP, AP down      */
    WIDF_ON_CONNECT_EVENT_ONLY,  /* fire STA_CONNECTED, host decides        */
} widf_on_connect_mode_t;

/* ── AP mode ─────────────────────────────────────────────────────────────── */
typedef enum {
    WIDF_AP_MODE_ON_DEMAND,   /* AP starts only when portal is triggered */
    WIDF_AP_MODE_ALWAYS,      /* AP always on (use when no STA expected)  */
} widf_ap_mode_t;

/* ── Configuration struct ────────────────────────────────────────────────── */
typedef struct {

    /* AP */
    char              ap_ssid[32];       /* Portal AP network name          */
    char              ap_password[64];   /* AP password, empty = open       */
    wifi_auth_mode_t  ap_authmode;       /* WIFI_AUTH_OPEN / WPA2 / WPA3    */

    /* Portal */
    uint32_t          portal_timeout_s;  /* Seconds before portal closes    */
    int               reopen_gpio;       /* 0 = reboot on timeout           */
    /* >0 = GPIO pin for long press    */
    uint32_t          long_press_ms;     /* Long press duration in ms       */

    /* Network access */
    bool              sta_access;        /* true  = portal always running,  */
    /*         AP only when no STA     */
    /* false = portal on demand only   */

    /* Device */
    char              hostname[32];      /* mDNS / DHCP hostname            */
    char              nvs_namespace[16]; /* NVS namespace for credentials   */
    uint8_t           max_sta_retries;   /* STA connection retry count      */

    /* Flow control */
    widf_fallback_mode_t   fallback_mode;   /* Behavior on connect failure  */
    widf_on_save_mode_t    on_save_mode;    /* Behavior after creds saved    */
    widf_on_connect_mode_t on_connect_mode; /* Behavior after STA connects   */

    /* Events */
    widf_event_cb_t   on_event;          /* NULL = no callback              */

} widf_mngr_config_t;

/* ── Default config macro ────────────────────────────────────────────────── */
#define WIDF_MNGR_DEFAULT_CONFIG() {                         \
.ap_ssid          = CONFIG_PORTAL_AP_SSID,               \
.ap_password      = "",                                  \
.ap_authmode      = WIFI_AUTH_OPEN,                      \
.portal_timeout_s = CONFIG_PORTAL_TIMEOUT_S,             \
.reopen_gpio      = CONFIG_PORTAL_BUTTON_GPIO,           \
.long_press_ms    = CONFIG_PORTAL_LONG_PRESS_MS,         \
.sta_access       = true,                                \
.hostname         = "widf",                              \
.nvs_namespace    = "wifi_creds",                        \
.max_sta_retries  = CONFIG_PORTAL_MAX_RETRY,             \
.fallback_mode    = WIDF_FALLBACK_AP_PORTAL,             \
.on_save_mode     = WIDF_ON_SAVE_RECONNECT,              \
.on_connect_mode  = WIDF_ON_CONNECT_AP_DOWN,             \
.on_event         = NULL,                                \
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Initialises WiFi, NVS, and GPIO, then runs the portal lifecycle.
 * Blocks until the portal closes. Call from app_main() or a task.
 * Pass NULL to use all Kconfig defaults. */
esp_err_t widf_mngr_run(const widf_mngr_config_t *config);

/* Returns the current STA IP as a string, or NULL if not connected.
 * Valid only while widf_mngr_run() is executing. */
const char *widf_mngr_get_sta_ip(void);

#endif /* WIDF_MNGR_H */
