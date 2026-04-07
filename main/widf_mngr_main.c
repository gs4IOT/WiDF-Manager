/*  widf_mngr_main.c — WIDF Manager system core
 *    WIDF Manager v0.7.5 — ESP-IDF native, ESP32 family 
 *
 *    Developed and tested on M5Stamp C3 (ESP32-C3). Compatible with any
 *    ESP32 variant — set PORTAL_BUTTON_GPIO in menuconfig to match your board.
 *
 *    Handles: NVS init, WiFi init (APSTA), STA connect + retry state machine,
 *    network scan, portal run loop, portal timeout, GPIO long press to
 *    reopen portal. HTTP handlers live in widf_mngr_handlers.c.
 *
 *    Boot sequence:
 *      1. Initialise NVS, GPIO (configurable pin), WiFi (APSTA mode) and event handlers
 *      2. If saved credentials found -> attempt STA connect, retry up to
 *         PORTAL_MAX_RETRY times, then fall through regardless of result
 *      3. Scan for nearby networks and run the portal
 *      4. Portal closes on timeout OR when /exit is hit
 *      5. After portal closes -> wait for button long press (configurable duration)
 *         On long press -> attempt STA reconnect -> reopen portal
 *
 *    HTTP routes (9 total) — see widf_mngr_handlers.c:
 *      GET  /              Main menu — WiFi status + navigation
 *      GET  /wifi          WiFi setup — scan results, select network, password
 *      GET  /wifi/refresh  Re-scan networks, redirect back to /wifi
 *      POST /save          Save SSID + password to NVS, reboot
 *      GET  /info          Device info — chip, RAM, flash, uptime, temp, WiFi
 *      GET  /erase         Erase WiFi credentials from NVS, reboot
 *      GET  /ota           OTA firmware update
 *      GET  /restart       Reboot the device
 *      GET  /exit          Shut down portal; long press button to reopen
 *
 *    Configuration (idf.py menuconfig -> WIDF Manager Configuration):
 *      PORTAL_AP_SSID       AP network name broadcast during portal mode
 *      PORTAL_AP_CHANNEL    WiFi channel for the AP (1-13)
 *      PORTAL_MAX_STA_CONN  Maximum simultaneous clients on the AP
 *      PORTAL_TIMEOUT_S     Seconds before the HTTP server auto-shuts down
 *      PORTAL_BUTTON_GPIO   GPIO pin for the portal reopen button (active low)
 *      PORTAL_LONG_PRESS_MS Hold duration in ms to trigger portal reopen
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdbool.h>
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "widf_mngr_handlers.h"

/* ── Kconfig — values set via idf.py menuconfig ──────────────────────────── */
#define PORTAL_AP_SSID       CONFIG_PORTAL_AP_SSID
#define PORTAL_AP_CHANNEL    CONFIG_PORTAL_AP_CHANNEL
#define PORTAL_MAX_STA_CONN  CONFIG_PORTAL_MAX_STA_CONN
#define PORTAL_TIMEOUT_S     CONFIG_PORTAL_TIMEOUT_S

#define PORTAL_MAX_RETRY     CONFIG_PORTAL_MAX_RETRY  /* set in menuconfig */
#define WIFI_SCAN_MAX_AP     20     /* Maximum APs kept from a single scan */

/* ── GPIO — configurable via menuconfig ──────────────────────────────────── */
/* Default: GPIO3 (G3) on M5Stamp C3, active low.
 *   Change PORTAL_BUTTON_GPIO in menuconfig to match your board:
 *     GPIO0  — most ESP32 DevKit boards
 *     GPIO9  — ESP32-C6 and ESP32-H2 DevKit boards  */
#define BUTTON_GPIO          CONFIG_PORTAL_BUTTON_GPIO
#define LONG_PRESS_MS        CONFIG_PORTAL_LONG_PRESS_MS
#define BUTTON_POLL_MS       50     /* polling interval in ms */

/* ── Globals ─────────────────────────────────────────────────────────────── */
static const char *TAG = "widf_mngr";

/* Scan options buffer — declared here, extern'd in widf_mngr_handlers.h */
char g_scan_options[6144] = {0};

static EventGroupHandle_t s_wifi_event_group;   /* signals connect/fail */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static int  s_retry_num      = 0;
static bool s_sta_connecting = false;

/* ── GPIO init ───────────────────────────────────────────────────────────── */
/* Configure the button pin as input with internal pull-up.
 *   Button pulls to GND when pressed (active low). */
static void gpio_init_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Button configured on GPIO%d (%d ms long press)",
             BUTTON_GPIO, LONG_PRESS_MS);
}

/* Blocks until the button is held LOW for LONG_PRESS_MS milliseconds.
 *   Polls every BUTTON_POLL_MS ms so the CPU can yield between checks.
 *   Returns only when a valid long press is detected. */
static void wait_for_long_press(void)
{
    ESP_LOGI(TAG, "Waiting for long press on GPIO%d to reopen portal...",
             BUTTON_GPIO);
    uint32_t held_ms = 0;
    while (true) {
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            held_ms += BUTTON_POLL_MS;
            if (held_ms >= LONG_PRESS_MS) {
                ESP_LOGI(TAG, "Long press detected — reopening portal");
                return;
            }
        } else {
            held_ms = 0;   /* reset if button released before threshold */
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

/* ── Event Handler ───────────────────────────────────────────────────────── */
/* Handles both WIFI_EVENT and IP_EVENT in a single callback.
 *   s_sta_connecting gates the STA retry logic so it never fires
 *   during AP-only mode or after the connect sequence has concluded. */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        	
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *e = event_data;
                ESP_LOGI(TAG, "Device " MACSTR " joined portal AP, AID=%d",
                         MAC2STR(e->mac), e->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *e = event_data;
                ESP_LOGI(TAG, "Device " MACSTR " left portal AP, AID=%d",
                         MAC2STR(e->mac), e->aid);
                break;
            }

            case WIFI_EVENT_STA_START:
                if (s_sta_connecting) esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (s_sta_connecting) {
                    if (s_retry_num < PORTAL_MAX_RETRY) {
                        s_retry_num++;
                        ESP_LOGW(TAG, "Connection failed, retrying (%d/%d)...",
                                 s_retry_num, PORTAL_MAX_RETRY);
                        esp_wifi_connect();
                    } else {
                        ESP_LOGE(TAG, "All %d attempts failed", PORTAL_MAX_RETRY);
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                }
                break;

            default:
                break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── NVS ─────────────────────────────────────────────────────────────────── */
/* Credentials are stored under the "wifi_creds" namespace.
 *   portal_nvs_load_creds() is called at boot and after each long press.
 *   Saving is handled by save_post_handler() in handlers.c.
 *   /erase calls nvs_flash_erase() which wipes the entire NVS partition. */
#define NVS_NAMESPACE  "wifi_creds"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "password"

static bool portal_nvs_load_creds(char *ssid, char *password)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t ssid_len = 33, pass_len = 65;
    esp_err_t err  = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    err |= nvs_get_str(handle, NVS_KEY_PASS, password, &pass_len);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    ESP_LOGI(TAG, "Credentials loaded. SSID: %s", ssid);
    return true;
}

/* ── WiFi init ───────────────────────────────────────────────────────────── */
/* wifi_init_common() — one-time driver init; does NOT call esp_wifi_start().
 *   wifi_configure_ap() — sets APSTA mode and AP config; called once at boot.
 *   STA config is set separately each time we attempt a connection. */
static void wifi_init_common(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
}

static void wifi_configure_ap(void)
{
    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = PORTAL_AP_CHANNEL,
            .max_connection = PORTAL_MAX_STA_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, PORTAL_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(PORTAL_AP_SSID);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

/* ── WiFi scan ───────────────────────────────────────────────────────────── */
/* Blocking scan — populates g_scan_options with HTML div elements for each AP.
 *   Hidden networks show BSSID instead of SSID and use the .hnet CSS class.
 *   WPA3 cases are guarded by CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT so the code
 *   compiles cleanly on targets or IDF versions without WPA3 support.
 *   Must be called after esp_wifi_start(). */
static const char *authmode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN:          return "Open";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
        #ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
        case WIFI_AUTH_WPA3_PSK:      return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
        #endif
        #ifdef CONFIG_ESP_WIFI_OWE_SUPPORT
        case WIFI_AUTH_OWE:           return "OWE";
        #endif
        default:                       return "?";
    }
}

static void wifi_scan(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true,
    };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > WIFI_SCAN_MAX_AP) ap_count = WIFI_SCAN_MAX_AP;

    wifi_ap_record_t records[WIFI_SCAN_MAX_AP];
    esp_wifi_scan_get_ap_records(&ap_count, records);

    g_scan_options[0] = '\0';
    for (int i = 0; i < ap_count; i++) {
        bool hidden  = (strlen((char *)records[i].ssid) == 0);
        int  bars    = (records[i].rssi >= -50) ? 4 :
        (records[i].rssi >= -60) ? 3 :
        (records[i].rssi >= -70) ? 2 : 1;
        const char *enc     = authmode_str(records[i].authmode);
        bool        is_open = (records[i].authmode == WIFI_AUTH_OPEN);

        char opt[384];
        if (hidden) {
            snprintf(opt, sizeof(opt),
                     "<div class='net hnet' onclick='selHidden(this)'>"
                     "<div class='netinfo'>"
                     "<span class='netname hidden'>Hidden (%02X:%02X:%02X:%02X:%02X:%02X)</span>"
                     "<div class='netmeta'>"
                     "<span class='dbm'>%d dBm</span>"
                     "<span class='enc%s'>%s</span>"
                     "</div></div>"
                     "<div class='signal b%d'><i></i><i></i><i></i><i></i></div>"
                     "</div>",
                     records[i].bssid[0], records[i].bssid[1], records[i].bssid[2],
                     records[i].bssid[3], records[i].bssid[4], records[i].bssid[5],
                     records[i].rssi, is_open ? " open" : "", enc, bars);
        } else {
            snprintf(opt, sizeof(opt),
                     "<div class='net' onclick='sel(this,\"%s\")'>"
                     "<div class='netinfo'>"
                     "<span class='netname'>%s</span>"
                     "<div class='netmeta'>"
                     "<span class='dbm'>%d dBm</span>"
                     "<span class='enc%s'>%s</span>"
                     "</div></div>"
                     "<div class='signal b%d'><i></i><i></i><i></i><i></i></div>"
                     "</div>",
                     records[i].ssid, records[i].ssid,
                     records[i].rssi, is_open ? " open" : "", enc, bars);
        }
        strncat(g_scan_options, opt,
                sizeof(g_scan_options) - strlen(g_scan_options) - 1);
    }
    ESP_LOGI(TAG, "Scan complete — %d networks found", ap_count);
}

/* ── STA connect ─────────────────────────────────────────────────────────── */
/* Attempt STA connection using saved credentials.
 *   Uses esp_wifi_connect() directly — wifi is already started in APSTA mode.
 *   Returns true if connected, false after PORTAL_MAX_RETRY failures. */
static bool try_sta_connect(const char *ssid, const char *password)
{
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,     sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    s_wifi_event_group = xEventGroupCreate();
    s_retry_num        = 0;
    s_sta_connecting   = true;

    esp_wifi_connect();   /* first attempt — subsequent retries via event handler */

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);
    s_sta_connecting = false;
    vEventGroupDelete(s_wifi_event_group);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ── Portal run ──────────────────────────────────────────────────────────── */
/* Scan, start the web server, then block until either:
 *   - PORTAL_TIMEOUT_S seconds elapse (idle timeout), or
 *   - g_exit_requested is set by exit_handler (/exit route).
 *   On timeout: reboots if PORTAL_REBOOT_ON_TIMEOUT is set, otherwise returns
 *   to app_main which waits for a long press.
 *   Stops the server before returning or rebooting either way. */
static void portal_run(void)
{
    g_exit_requested = false;
    wifi_scan();
    start_webserver();

    ESP_LOGI(TAG, "Portal open — timeout in %d s, or press Exit in browser",
             PORTAL_TIMEOUT_S);

    uint32_t elapsed_ms = 0;
    uint32_t timeout_ms = (uint32_t)PORTAL_TIMEOUT_S * 1000;

    while (elapsed_ms < timeout_ms) {
        if (g_exit_requested) {
            ESP_LOGW(TAG, "Exit requested via browser");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        elapsed_ms += 500;
    }

    if (!g_exit_requested) {
        ESP_LOGW(TAG, "Portal timeout reached");
    }

    if (g_server) {
        httpd_stop(g_server);
        g_server = NULL;
    }

#if CONFIG_PORTAL_REBOOT_ON_TIMEOUT
    if (!g_exit_requested) {
        ESP_LOGW(TAG, "PORTAL_REBOOT_ON_TIMEOUT enabled — rebooting");
        esp_restart();
    }
#endif
}

/* ── Main ────────────────────────────────────────────────────────────────── */
/* Boot sequence:
 *   1. NVS init, GPIO button init, WiFi common init + AP config
 *   2. esp_wifi_start() — APSTA mode, AP always up
 *   3. If saved credentials exist -> try STA connect
 *   4. Run portal (scan + HTTP server + timeout/exit loop)
 *   5. Portal closed -> wait for button long press
 *   6. On long press -> try STA reconnect -> reopen portal (goto 4) */
void app_main(void)
{
    /* Mark this boot as successful — prevents rollback to previous firmware
     *       Must be called early in app_main after a successful OTA update boots. */
    esp_ota_mark_app_valid_cancel_rollback();

    /* NVS — erase and reinit if partition is full or firmware was updated */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_init_button();
    wifi_init_common();
    wifi_configure_ap();
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Initial STA connect attempt */
    char ssid[33]     = {0};
    char password[65] = {0};
    if (portal_nvs_load_creds(ssid, password)) {
        ESP_LOGI(TAG, "Trying saved credentials for: %s", ssid);
        if (try_sta_connect(ssid, password)) {
            ESP_LOGI(TAG, "Connected to \"%s\" — portal also running", ssid);
#if !CONFIG_PORTAL_KEEP_AP_ACTIVE
            /* Switch to pure STA — shuts down the AP to reduce RF footprint */
            ESP_LOGI(TAG, "PORTAL_KEEP_AP_ACTIVE disabled — stopping AP");
            esp_wifi_set_mode(WIFI_MODE_STA);
#endif
        } else {
            ESP_LOGW(TAG, "Could not connect to \"%s\" — portal open for new credentials", ssid);
        }
    } else {
        ESP_LOGI(TAG, "No saved credentials — starting portal");
    }

    /* Main loop — portal -> wait for long press -> reconnect -> repeat */
    while (true) {
        portal_run();

        /* Portal is now closed — check if we have a connection */
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "Still connected to \"%s\" — waiting for long press on GPIO%d",
                     (char *)ap_info.ssid, BUTTON_GPIO);
        } else {
            ESP_LOGW(TAG, "Not connected — waiting for long press on GPIO%d to reopen portal",
                     BUTTON_GPIO);
        }

        wait_for_long_press();

        /* Try to reconnect with saved credentials before reopening portal */
        memset(ssid,     0, sizeof(ssid));
        memset(password, 0, sizeof(password));
        if (portal_nvs_load_creds(ssid, password)) {
            ESP_LOGI(TAG, "Long press: trying STA reconnect to \"%s\"", ssid);
            if (try_sta_connect(ssid, password)) {
                ESP_LOGI(TAG, "Reconnected to \"%s\"", ssid);
            } else {
                ESP_LOGW(TAG, "Reconnect failed — opening portal for new credentials");
            }
        } else {
            ESP_LOGI(TAG, "Long press: no saved credentials — opening portal");
        }
        /* Loop back to portal_run() */
    }
}

/* ── End of widf_mngr_main.c ─────────────────────────────────────────────── */
