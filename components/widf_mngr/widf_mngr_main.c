/*  widf_mngr_main.c — WIDF Manager system core
 *    WIDF Manager v0.9.0 — ESP-IDF native, ESP32 family
 *
 *    Developed and tested on M5Stamp C3 (ESP32-C3). Compatible with any
 *    ESP32 variant — set reopen_gpio in widf_mngr_config_t to match your board.
 *
 *    Handles: NVS init, WiFi init (APSTA), STA connect + retry state machine,
 *    network scan, portal run loop, portal timeout, GPIO long press to
 *    reopen portal. HTTP handlers live in widf_mngr_handlers.c.
 *
 *    Boot sequence:
 *      1. Initialise NVS, GPIO, WiFi (APSTA mode) and event handlers
 *      2. If saved credentials found -> attempt STA connect, retry up to
 *         config->max_sta_retries times, then fall through regardless of result
 *      3. Scan for nearby networks and run the portal
 *      4. Portal closes on timeout OR when /exit is hit
 *      5. On timeout: reboot if reopen_gpio == 0, else wait for long press
 *      6. On long press -> attempt STA reconnect -> reopen portal
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
#include "mdns.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "widf_mngr.h"
#include "widf_mngr_handlers.h"
#if CONFIG_PORTAL_DNS_ENABLED
#include "widf_mngr_dns.h"
#endif

/* ── Constants ───────────────────────────────────────────────────────────── */
#define WIFI_SCAN_MAX_AP   20
#define BUTTON_POLL_MS     50

/* ── Globals ─────────────────────────────────────────────────────────────── */
static const char *TAG = "widf_mngr";

/* Active config — set at start of widf_mngr_run(), used by callbacks */
static const widf_mngr_config_t *s_cfg = NULL;

/* Scan options buffer — extern'd in widf_mngr_handlers.h */
char g_scan_options[6144] = {0};

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static int  s_retry_num      = 0;
static bool s_sta_connecting = false;

/* STA IP — updated on connect, returned by widf_mngr_get_sta_ip() */
static char s_sta_ip[16] = {0};

/* ── Public API helpers ──────────────────────────────────────────────────── */
const char *widf_mngr_get_sta_ip(void)
{
    return (s_sta_ip[0] != '\0') ? s_sta_ip : NULL;
}

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(s_cfg->hostname));
    ESP_ERROR_CHECK(mdns_instance_name_set("WIDF Manager"));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started — reachable at %s.local", s_cfg->hostname);
}

/* ── GPIO ────────────────────────────────────────────────────────────────── */
static void gpio_init_button(int gpio_pin)
{
    if (gpio_pin <= 0) return;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio_pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Button configured on GPIO%d (%lu ms long press)",
             gpio_pin, (unsigned long)s_cfg->long_press_ms);
}

static void wait_for_long_press(int gpio_pin, uint32_t long_press_ms)
{
    ESP_LOGI(TAG, "Waiting for long press on GPIO%d to reopen portal...",
             gpio_pin);
    uint32_t held_ms = 0;
    while (true) {
        if (gpio_get_level(gpio_pin) == 0) {
            held_ms += BUTTON_POLL_MS;
            if (held_ms >= long_press_ms) {
                ESP_LOGI(TAG, "Long press detected — reopening portal");
                return;
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

/* ── Event Handler ───────────────────────────────────────────────────────── */
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
                    uint8_t max_retry = s_cfg ? s_cfg->max_sta_retries : 3;
                    if (s_retry_num < max_retry) {
                        s_retry_num++;
                        ESP_LOGW(TAG, "Connection failed, retrying (%d/%d)...",
                                 s_retry_num, max_retry);
                        esp_wifi_connect();
                    } else {
                        ESP_LOGE(TAG, "All %d attempts failed", max_retry);
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }
                }
                break;

            default:
                break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "Connected! IP: %s", s_sta_ip);
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── NVS ─────────────────────────────────────────────────────────────────── */
#define NVS_KEY_SSID  "ssid"
#define NVS_KEY_PASS  "password"

static bool portal_nvs_load_creds(char *ssid, char *password)
{
    const char *ns = s_cfg ? s_cfg->nvs_namespace : "wifi_creds";
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t ssid_len = 33, pass_len = 65;
    esp_err_t err  = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssid_len);
    err |= nvs_get_str(handle, NVS_KEY_PASS, password, &pass_len);
    nvs_close(handle);
    if (err != ESP_OK) return false;
    ESP_LOGI(TAG, "Credentials loaded. SSID: %s", ssid);
    return true;
}

/* ── WiFi init ───────────────────────────────────────────────────────────── */
static void wifi_init_common(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* Set hostname on STA interface */
    if (s_cfg && s_cfg->hostname[0] != '\0') {
        esp_netif_set_hostname(sta_netif, s_cfg->hostname);
    }

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
            .channel        = CONFIG_PORTAL_AP_CHANNEL,
            .max_connection = CONFIG_PORTAL_MAX_STA_CONN,
            .authmode       = s_cfg->ap_authmode,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, s_cfg->ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(s_cfg->ap_ssid);

    if (s_cfg->ap_authmode != WIFI_AUTH_OPEN) {
        strncpy((char *)ap_cfg.ap.password, s_cfg->ap_password,
                sizeof(ap_cfg.ap.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

/* ── WiFi scan ───────────────────────────────────────────────────────────── */
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
    build_scan_options(records, ap_count);
    ESP_LOGI(TAG, "Scan complete — %d networks found", ap_count);
}

/* ── STA connect ─────────────────────────────────────────────────────────── */
static bool try_sta_connect(const char *ssid, const char *password)
{
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,     sizeof(sta_cfg.sta.ssid)     - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    s_wifi_event_group = xEventGroupCreate();
    s_retry_num        = 0;
    s_sta_connecting   = true;

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);
    s_sta_connecting = false;
    vEventGroupDelete(s_wifi_event_group);

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ── Portal run ──────────────────────────────────────────────────────────── */
static void portal_run(void)
{
    g_exit_requested = false;
    wifi_scan();
#if CONFIG_PORTAL_DNS_ENABLED
    dns_server_start();
#endif
    start_webserver();

    ESP_LOGI(TAG, "Portal open — timeout in %lu s, or press Exit in browser",
             (unsigned long)s_cfg->portal_timeout_s);

    uint32_t elapsed_ms = 0;
    uint32_t timeout_ms = s_cfg->portal_timeout_s * 1000;

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
#if CONFIG_PORTAL_DNS_ENABLED
    dns_server_stop();
#endif
}

/* ── Public API ──────────────────────────────────────────────────────────── */
esp_err_t widf_mngr_run(const widf_mngr_config_t *config)
{
    /* Use default config if NULL passed */
    widf_mngr_config_t default_cfg = WIDF_MNGR_DEFAULT_CONFIG();
    s_cfg = config ? config : &default_cfg;

    /* Mark OTA boot as valid */
    esp_ota_mark_app_valid_cancel_rollback();

    /* NVS init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* GPIO init — skip if reopen_gpio == 0 (headless/reboot mode) */
    gpio_init_button(s_cfg->reopen_gpio);

    /* WiFi init */
    wifi_init_common();
    wifi_configure_ap();
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Initial STA connect attempt */
    char ssid[33]     = {0};
    char password[65] = {0};
    bool connected    = false;

    if (portal_nvs_load_creds(ssid, password)) {
        ESP_LOGI(TAG, "Trying saved credentials for: %s", ssid);
        connected = try_sta_connect(ssid, password);
        if (connected) {
            ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
            mdns_start();
            if (!s_cfg->sta_access) {
                /* sta_access off: shut AP down after STA connects */
                esp_wifi_set_mode(WIFI_MODE_STA);
                ESP_LOGI(TAG, "sta_access disabled — AP stopped");
            }
        } else {
            ESP_LOGW(TAG, "Could not connect to \"%s\" — opening portal", ssid);
        }
    } else {
        ESP_LOGI(TAG, "No saved credentials — starting portal");
    }

    /* Main loop */
    while (true) {
        /* sta_access mode: portal always runs, AP only up when not connected */
        if (s_cfg->sta_access && connected) {
            /* Keep portal running on STA IP, no AP needed */
            ESP_LOGI(TAG, "sta_access: portal running on STA IP %s", s_sta_ip);
        }

        portal_run();

        /* After portal closes */
        wifi_ap_record_t ap_info;
        connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

        if (s_cfg->reopen_gpio <= 0) {
            /* Headless mode — reboot on timeout */
            ESP_LOGW(TAG, "reopen_gpio not set — rebooting");
            esp_restart();
        }

        /* GPIO mode — wait for long press */
        if (connected) {
            ESP_LOGI(TAG, "Still connected to \"%s\" — waiting for long press on GPIO%d",
                     (char *)ap_info.ssid, s_cfg->reopen_gpio);
        } else {
            ESP_LOGW(TAG, "Not connected — waiting for long press on GPIO%d",
                     s_cfg->reopen_gpio);
        }

        wait_for_long_press(s_cfg->reopen_gpio, s_cfg->long_press_ms);

        /* Reconnect attempt before reopening portal */
        memset(ssid,     0, sizeof(ssid));
        memset(password, 0, sizeof(password));
        if (portal_nvs_load_creds(ssid, password)) {
            ESP_LOGI(TAG, "Long press: trying STA reconnect to \"%s\"", ssid);
            connected = try_sta_connect(ssid, password);
            if (connected) {
                ESP_LOGI(TAG, "Reconnected to \"%s\"", ssid);
                mdns_start();
            } else {
                ESP_LOGW(TAG, "Reconnect failed — opening portal for new credentials");
            }
        } else {
            ESP_LOGI(TAG, "Long press: no saved credentials — opening portal");
        }
    }

    return ESP_OK;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */
void app_main(void)
{
    widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();
    widf_mngr_run(&cfg);
}

/* ── End of widf_mngr_main.c ─────────────────────────────────────────────── */