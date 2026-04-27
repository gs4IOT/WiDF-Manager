/*  widf_mngr_handlers.c — HTTP handlers for WIDF Manager
 *    WIDF Manager v1.1.3 — ESP-IDF native, ESP32 family
 *
 *    All httpd URI handler functions and start_webserver() live here.
 *    System logic (WiFi, NVS, scan, app_main) stays in widf_mngr_main.c.
 *
 *    Portability notes:
 *      - Temperature sensor guarded by CONFIG_SOC_TEMP_SENSOR_SUPPORTED —
 *        displays "N/A" on chips without an internal sensor (e.g. original ESP32)
 *      - WPA3 authmode cases guarded by CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
 *      - Chip model resolved at runtime from esp_chip_info() model enum
 *
 *    Handlers:
 *      menu_get_handler     GET /              Main menu with live status
 *      portal_get_handler   GET /wifi          WiFi setup page
 *      wifi_refresh_handler GET /wifi/refresh  Re-scan and redirect
 *      save_post_handler    POST /save         Save credentials, reboot
 *      info_get_handler     GET /info          Live device + WiFi info
 *      erase_handler        GET /erase         Wipe NVS credentials, reboot
 *      ota_get_handler      GET /ota           OTA placeholder
 *      restart_handler      GET /restart       Reboot
 *      exit_handler         GET /exit          Stop server, set exit flag
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#ifdef CONFIG_SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "widf_mngr_handlers.h"
#include "widf_mngr.h"

static const char *TAG = "widf_handlers";

/* ── Shared globals ──────────────────────────────────────────────────────── */
/* g_server is stored here so exit_handler can signal a stop.
 *   g_exit_requested is polled by portal_run() in widf_mngr_main.c. */
httpd_handle_t g_server         = NULL;
volatile bool  g_exit_requested = false;
volatile bool  g_reconnect_requested = false;
char           g_reconnect_ssid[33]  = {0};
char           g_reconnect_pass[65]  = {0};

/* ── Kconfig alias ───────────────────────────────────────────────────────── */
#define PORTAL_AP_SSID  CONFIG_PORTAL_AP_SSID

/* ── NVS ─────────────────────────────────────────────────────────────────── */
#define NVS_NAMESPACE  "wifi_creds"
#define WIDF_MAX_NETWORKS  3
#define NVS_KEY_NET_COUNT  "net_count"

/* Saves credentials using multi-network indexed NVS storage.
 *   - If SSID already exists: update password in place
 *   - If new: shift existing entries down, write at index 0
 *   - Maximum WIDF_MAX_NETWORKS entries, oldest dropped on overflow */
static bool portal_nvs_save_creds(const char *ssid, const char *password)
{
    extern const widf_mngr_config_t *widf_mngr_get_config(void);
    const widf_mngr_config_t *mcfg = widf_mngr_get_config();
    const char *ns = mcfg ? mcfg->nvs_namespace : NVS_NAMESPACE;
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) return false;

    /* Read current count */
    uint8_t count = 0;
    nvs_get_u8(handle, NVS_KEY_NET_COUNT, &count);
    if (count > WIDF_MAX_NETWORKS) count = WIDF_MAX_NETWORKS;

    /* Check if SSID already exists — update password in place */
    char key_ssid[12], key_pass[12];
    char stored_ssid[33] = {0};
    size_t ssid_len = sizeof(stored_ssid);

    for (int i = 0; i < count; i++) {
        snprintf(key_ssid, sizeof(key_ssid), "ssid_%d", i);
        ssid_len = sizeof(stored_ssid);
        if (nvs_get_str(handle, key_ssid, stored_ssid, &ssid_len) == ESP_OK) {
            if (strcmp(stored_ssid, ssid) == 0) {
                snprintf(key_pass, sizeof(key_pass), "pass_%d", i);
                nvs_set_str(handle, key_pass, password);
                nvs_commit(handle);
                nvs_close(handle);
                ESP_LOGI(TAG, "Updated password for existing SSID: %s", ssid);
                return true;
            }
        }
    }

    /* New SSID — shift entries down, cap at WIDF_MAX_NETWORKS */
    int new_count = (count < WIDF_MAX_NETWORKS) ? count + 1 : WIDF_MAX_NETWORKS;
    int shift_to  = new_count - 1;

    /* Shift from bottom up to avoid overwriting */
    for (int i = shift_to; i > 0; i--) {
        char src_ssid[12], src_pass[12];
        char dst_ssid[12], dst_pass[12];
        char val_ssid[33] = {0}, val_pass[65] = {0};
        size_t val_len;

        snprintf(src_ssid, sizeof(src_ssid), "ssid_%d", i - 1);
        snprintf(src_pass, sizeof(src_pass), "pass_%d", i - 1);
        snprintf(dst_ssid, sizeof(dst_ssid), "ssid_%d", i);
        snprintf(dst_pass, sizeof(dst_pass), "pass_%d", i);

        val_len = sizeof(val_ssid);
        if (nvs_get_str(handle, src_ssid, val_ssid, &val_len) == ESP_OK)
            nvs_set_str(handle, dst_ssid, val_ssid);

        val_len = sizeof(val_pass);
        if (nvs_get_str(handle, src_pass, val_pass, &val_len) == ESP_OK)
            nvs_set_str(handle, dst_pass, val_pass);
    }

    /* Write new entry at index 0 */
    nvs_set_str(handle, "ssid_0", ssid);
    nvs_set_str(handle, "pass_0", password);
    nvs_set_u8(handle,  NVS_KEY_NET_COUNT, (uint8_t)new_count);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Saved new network at index 0: %s (%d/%d stored)",
             ssid, new_count, WIDF_MAX_NETWORKS);
    return true;
}

/* ── Embedded HTML ───────────────────────────────────────────────────────── */
extern const char portal_top_html_start[]    asm("_binary_Top_portal_html_start");
extern const char portal_top_html_end[]      asm("_binary_Top_portal_html_end");
extern const char portal_bottom_html_start[] asm("_binary_Bottom_portal_html_start");
extern const char portal_bottom_html_end[]   asm("_binary_Bottom_portal_html_end");

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Decodes a URL-encoded string from an HTTP POST body.
 *   Handles '+' -> space and '%XX' -> byte. Output is always null-terminated. */
static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t i = 0;
    while (*src && i < dst_len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/* Writes a live WiFi status banner into buf for injection into the /wifi page.
 *   Green "Connected to: SSID" if STA is up, amber "AP mode" otherwise. */
static void build_status_chunk(char *buf, size_t len)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(buf, len,
                 "<div class='status ok'>Connected to: %s</div>", ap_info.ssid);
    } else {
        snprintf(buf, len,
                 "<div class='status off'>Access Point mode &#8212; no network connected</div>");
    }
}

/* GET /generate_204 — Android captive portal detection.
 * Return 302 redirect to portal instead of 204, triggering the popup. */
esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* Maps esp_chip_model_t to a human-readable string.
 *   Covers all ESP32 variants known in IDF v6.x.
 *   New variants added in IDF v5.1+: C6, C5, P4, C61.
 *   Guarded by target macros so the file compiles on any target. */
static const char *chip_model_str(esp_chip_model_t model)
{
    switch (model) {
        case CHIP_ESP32:    return "ESP32";
        case CHIP_ESP32S2:  return "ESP32-S2";
        case CHIP_ESP32S3:  return "ESP32-S3";
        case CHIP_ESP32C3:  return "ESP32-C3";
        case CHIP_ESP32C2:  return "ESP32-C2";
        case CHIP_ESP32H2:  return "ESP32-H2";
        #ifdef CONFIG_IDF_TARGET_ESP32C6
        case CHIP_ESP32C6:  return "ESP32-C6";
        #endif
        #ifdef CONFIG_IDF_TARGET_ESP32C5
        case CHIP_ESP32C5:  return "ESP32-C5";
        #endif
        #ifdef CONFIG_IDF_TARGET_ESP32P4
        case CHIP_ESP32P4:  return "ESP32-P4";
        #endif
        #ifdef CONFIG_IDF_TARGET_ESP32C61
        case CHIP_ESP32C61: return "ESP32-C61";
        #endif
        default:            return "ESP32 (unknown)";
    }
}

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

void build_scan_options(wifi_ap_record_t *records, uint16_t count)
{
    g_scan_options[0] = '\0';
    for (int i = 0; i < count; i++) {
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
}

/* ── HTTP Handlers ───────────────────────────────────────────────────────── */

/* GET /favicon.ico — return 204 No Content to suppress browser 404 noise */
esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* GET / — main menu
 *   Shows live STA connection status and navigation buttons to all pages.
 *   Page is built in two snprintf passes (body + page) to stay within
 *   the 3072-byte stack buffer. */
esp_err_t menu_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap_info;
    char status[200] = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(status, sizeof(status),
                 "<div style='font-size:.85rem;font-weight:600;padding:10px 14px;"
                 "border-radius:10px;margin-bottom:20px;"
                 "background:#e6f4ea;color:#2d6a4f'>Connected to: %s</div>",
                 ap_info.ssid);
    } else {
        snprintf(status, sizeof(status),
                 "<div style='font-size:.85rem;font-weight:600;padding:10px 14px;"
                 "border-radius:10px;margin-bottom:20px;"
                 "background:#fff3e0;color:#b5590a'>"
                 "Not connected to any network</div>");
    }

    char body[2048] = {0};
    snprintf(body, sizeof(body),
             "<div class='card' style='text-align:left'>"
             "<h2 style='font-size:1.4rem;margin-bottom:4px'>WIDF Manager</h2>"
             "<p style='margin-bottom:16px'>%s</p>"
             "%s"
             "<a class='btn pri' href='/wifi'>&#x1F4F6;&nbsp; WiFi Setup</a>"
             "<a class='btn sec' href='/info'>&#x2139;&#xFE0F;&nbsp; Info</a>"
             "<a class='btn sec' href='/ota'>&#x2B06;&#xFE0F;&nbsp; OTA Update</a>"
             "<hr style='border:none;border-top:1px solid #eee;margin:14px 0'>"
             "<a class='btn sec' href='/restart'>&#x1F504;&nbsp; Reboot</a>"
             "<a class='btn warn' href='/exit'>&#x274C;&nbsp; Exit Portal</a>"
             "</div></body></html>",
             PORTAL_AP_SSID, status);

    char page[3072] = {0};
    snprintf(page, sizeof(page), "%s%s", PAGE_HEAD("WIDF Manager"), body);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, page);
    return ESP_OK;
}

/* GET /wifi — WiFi setup page
 *   Serves Top_portal.html + live status banner + g_scan_options + Bottom_portal.html
 *   as a single chunked response. Scan results are pre-built by wifi_scan(). */
esp_err_t portal_get_handler(httpd_req_t *req)
{
    char status_buf[128] = {0};
    build_status_chunk(status_buf, sizeof(status_buf));

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, portal_top_html_start,
                          portal_top_html_end - portal_top_html_start);
    httpd_resp_send_chunk(req, status_buf, strlen(status_buf));
    httpd_resp_send_chunk(req, g_scan_options, strlen(g_scan_options));
    httpd_resp_send_chunk(req, portal_bottom_html_start,
                          portal_bottom_html_end - portal_bottom_html_start);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /wifi/refresh — trigger a fresh network scan then redirect back to /wifi
 *   WPA3 authmode cases guarded by CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT. */
esp_err_t wifi_refresh_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = true,
    };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t records[20];
    esp_wifi_scan_get_ap_records(&ap_count, records);
    build_scan_options(records, ap_count);
    ESP_LOGI(TAG, "Refresh scan complete — %d networks found", ap_count);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/wifi");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
/* POST /save — receive URL-encoded form body, decode SSID + password,
 *   save to NVS, then behave according to on_save_mode:
 *     WIDF_ON_SAVE_RECONNECT  — set reconnect flag, portal_run() handles it
 *     WIDF_ON_SAVE_EVENT_ONLY — fire event only, host decides next step     */
esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int  len      = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    char ssid_raw[33] = {0}, password_raw[65] = {0};
    char ssid[33]     = {0}, password[65]      = {0};
    httpd_query_key_value(buf, "ssid",     ssid_raw,     sizeof(ssid_raw));
    httpd_query_key_value(buf, "password", password_raw, sizeof(password_raw));
    url_decode(ssid_raw,     ssid,     sizeof(ssid));
    url_decode(password_raw, password, sizeof(password));

    ESP_LOGI(TAG, "Saving credentials for SSID: %s", ssid);
    portal_nvs_save_creds(ssid, password);
    widf_mngr_notify_saved(ssid);

    /* Retrieve on_save_mode via extern config accessor */
    extern const widf_mngr_config_t *widf_mngr_get_config(void);
    const widf_mngr_config_t *cfg = widf_mngr_get_config();
    widf_on_save_mode_t on_save = cfg ? cfg->on_save_mode : WIDF_ON_SAVE_RECONNECT;

    if (on_save == WIDF_ON_SAVE_RECONNECT) {
        /* Signal portal_run() to close portal and attempt reconnect */
        strncpy(g_reconnect_ssid, ssid,     sizeof(g_reconnect_ssid) - 1);
        strncpy(g_reconnect_pass, password, sizeof(g_reconnect_pass) - 1);
        g_reconnect_requested = true;

        const char *resp =
        PAGE_HEAD("Connecting...")
        "<div class='card'>"
        "<h2>Connecting...</h2>"
        "<p>Credentials saved.<br>"
        "The device is now attempting to connect.</p>"
        "</div></body></html>";
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, resp);

    } else {
        /* WIDF_ON_SAVE_EVENT_ONLY — event already fired, just confirm */
        const char *resp =
        PAGE_HEAD("Saved!")
        "<div class='card'>"
        "<h2 style='color:#2d6a4f'>&#10003; Saved!</h2>"
        "<p>Credentials saved.</p>"
        "</div></body></html>";
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, resp);
    }

    return ESP_OK;
}

/* GET /info — live device and WiFi information page.
 *   Temperature reading is conditionally compiled — on chips without an internal
 *   sensor (CONFIG_SOC_TEMP_SENSOR_SUPPORTED not set) the row shows "N/A".
 *   Chip model is resolved at runtime from esp_chip_info() so this handler
 *   works correctly on any ESP32 variant without recompilation. */
esp_err_t info_get_handler(httpd_req_t *req)
{
    /* ── Chip info ── */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *chip_model = chip_model_str(chip.model);

    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    char chip_id[18] = {0};
    snprintf(chip_id, sizeof(chip_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* ── Flash ── */
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint32_t sketch_size = running ? running->size : 0;

    /* ── RAM ── */
    uint32_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t ram_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t ram_used  = ram_total - ram_free;

    /* ── CPU frequency — compile-time constant from sdkconfig ── */
    int cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    /* ── Uptime ── */
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    int     up_mins  = (int)(uptime_s / 60);
    int     up_secs  = (int)(uptime_s % 60);

    /* ── Temperature — guarded: only available on supported chips ── */
    char temp_str[16] = "N/A";
    #ifdef CONFIG_SOC_TEMP_SENSOR_SUPPORTED
    float temperature = 0.0f;
    temperature_sensor_handle_t temp_sensor = NULL;
    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&temp_cfg, &temp_sensor) == ESP_OK) {
        temperature_sensor_enable(temp_sensor);
        temperature_sensor_get_celsius(temp_sensor, &temperature);
        temperature_sensor_disable(temp_sensor);
        temperature_sensor_uninstall(temp_sensor);
        snprintf(temp_str, sizeof(temp_str), "%.1f &deg;C", temperature);
    }
    #endif

    /* ── WiFi ── */
    wifi_ap_record_t sta_info  = {0};
    bool             connected = (esp_wifi_sta_get_ap_info(&sta_info) == ESP_OK);

    char sta_ip[16]      = "0.0.0.0";
    char sta_gw[16]      = "0.0.0.0";
    char sta_nm[16]      = "0.0.0.0";
    char dns_str[16]     = "0.0.0.0";
    char sta_mac_str[18] = {0};
    char ap_mac_str[18]  = {0};
    const char *hostname = "";

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t *ap_netif  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (sta_netif) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ip_info.ip));
            snprintf(sta_gw, sizeof(sta_gw), IPSTR, IP2STR(&ip_info.gw));
            snprintf(sta_nm, sizeof(sta_nm), IPSTR, IP2STR(&ip_info.netmask));
        }
        esp_netif_dns_info_t dns_info = {0};
        if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            snprintf(dns_str, sizeof(dns_str), IPSTR,
                     IP2STR(&dns_info.ip.u_addr.ip4));
        }
        esp_netif_get_hostname(sta_netif, &hostname);
    }

    uint8_t sta_mac[6] = {0}, ap_mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
    esp_wifi_get_mac(WIFI_IF_AP,  ap_mac);
    snprintf(sta_mac_str, sizeof(sta_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    snprintf(ap_mac_str, sizeof(ap_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4], ap_mac[5]);

    char ap_ip_str[16] = "192.168.4.1";
    if (ap_netif) {
        esp_netif_ip_info_t ap_ip = {0};
        if (esp_netif_get_ip_info(ap_netif, &ap_ip) == ESP_OK) {
            snprintf(ap_ip_str, sizeof(ap_ip_str), IPSTR, IP2STR(&ap_ip.ip));
        }
    }

    /* ── About ── */
    const esp_app_desc_t *app_desc = esp_app_get_description();

    /* ── Build page ── */
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    httpd_resp_sendstr_chunk(req,
                             "<!DOCTYPE html><html><head>"
                             "<meta charset='utf-8'>"
                             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                             "<title>WIDF Manager &#8212; Info</title>"
                             "<style>"
                             "*{box-sizing:border-box;margin:0;padding:0}"
                             "body{font-family:-apple-system,sans-serif;background:#f0f2f5;padding:16px}"
                             ".card{background:#fff;border-radius:16px;"
                             "box-shadow:0 4px 24px rgba(0,0,0,.10);"
                             "padding:22px 20px;width:100%;max-width:480px;margin:0 auto 14px auto}"
                             "h2{font-size:1.1rem;font-weight:700;color:#1a1a2e;"
                             "margin-bottom:14px;padding-bottom:8px;border-bottom:1px solid #eee}"
                             ".row{display:flex;justify-content:space-between;"
                             "align-items:baseline;padding:6px 0;"
                             "border-bottom:1px solid #f5f5f5;font-size:.88rem}"
                             ".row:last-child{border-bottom:none}"
                             ".lbl{color:#888;font-weight:500;flex-shrink:0;margin-right:12px}"
                             ".val{color:#222;font-weight:600;text-align:right;word-break:break-all}"
                             ".ok{color:#2d6a4f}.off{color:#b5590a}"
                             ".btn{display:block;width:100%;max-width:480px;margin:0 auto 10px;"
                             "padding:13px;border:none;border-radius:10px;"
                             "font-size:.95rem;font-weight:600;cursor:pointer;"
                             "text-align:center;text-decoration:none}"
                             ".sec{background:#f0f2f5;color:#444}"
                             ".warn{background:#fff0f0;color:#c0392b}"
                             "table{width:100%;border-collapse:collapse;font-size:.83rem;margin-top:8px}"
                             "th{text-align:left;color:#888;font-weight:600;"
                             "padding:4px 8px 4px 0;border-bottom:1px solid #eee}"
                             "td{padding:5px 8px 5px 0;vertical-align:top;color:#444}"
                             "td:first-child{font-weight:600;color:#222;white-space:nowrap}"
                             "</style></head><body>");

    /* Device card — temperature row uses temp_str which is "N/A" on unsupported chips */
    char card[1536] = {0};
    snprintf(card, sizeof(card),
             "<div class='card'><h2>Device</h2>"
             "<div class='row'><span class='lbl'>Chip</span><span class='val'>%s rev %d</span></div>"
             "<div class='row'><span class='lbl'>Chip ID</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>CPU</span><span class='val'>%d MHz</span></div>"
             "<div class='row'><span class='lbl'>Flash</span><span class='val'>%lu KB total / %lu KB sketch</span></div>"
             "<div class='row'><span class='lbl'>RAM</span><span class='val'>%lu KB used / %lu KB free</span></div>"
             "<div class='row'><span class='lbl'>Uptime</span><span class='val'>%d min %d sec</span></div>"
             "<div class='row'><span class='lbl'>Temperature</span><span class='val'>%s</span></div>"
             "</div>",
             chip_model, chip.revision, chip_id, cpu_freq_mhz,
             (unsigned long)(flash_size / 1024), (unsigned long)(sketch_size / 1024),
             (unsigned long)(ram_used / 1024), (unsigned long)(ram_free / 1024),
             up_mins, up_secs, temp_str);
    httpd_resp_sendstr_chunk(req, card);

    /* WiFi card */
    char wifi_card[2048] = {0};
    snprintf(wifi_card, sizeof(wifi_card),
             "<div class='card'><h2>WiFi</h2>"
             "<div class='row'><span class='lbl'>Status</span><span class='val %s'>%s</span></div>"
             "<div class='row'><span class='lbl'>SSID</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>RSSI</span><span class='val'>%d dBm</span></div>"
             "<div class='row'><span class='lbl'>Station IP</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>Gateway</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>Subnet</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>DNS</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>Hostname</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>Station MAC</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>AP IP</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>AP MAC</span><span class='val'>%s</span></div>"
             "</div>",
             connected ? "ok" : "off",
             connected ? "Connected" : "Not connected",
             connected ? (char *)sta_info.ssid : "-",
             connected ? sta_info.rssi : 0,
             sta_ip, sta_gw, sta_nm, dns_str,
             hostname ? hostname : "-",
             sta_mac_str, ap_ip_str, ap_mac_str);
    httpd_resp_sendstr_chunk(req, wifi_card);

    /* About card */
    char about[2048] = {0};
    snprintf(about, sizeof(about),
             "<div class='card'><h2>About</h2>"
             "<div class='row'><span class='lbl'>App version</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>IDF version</span><span class='val'>%s</span></div>"
             "<div class='row'><span class='lbl'>Build date</span><span class='val'>%s %s</span></div>"
             "<table>"
             "<tr><th>Page</th><th>Description</th></tr>"
             "<tr><td>/</td><td>Menu page</td></tr>"
             "<tr><td>/wifi</td><td>WiFi setup &#8212; scan and connect</td></tr>"
             "<tr><td>/wifi/refresh</td><td>Re-scan networks</td></tr>"
             "<tr><td>/save</td><td>Save WiFi credentials and reboot</td></tr>"
             "<tr><td>/info</td><td>This page</td></tr>"
             "<tr><td>/ota</td><td>OTA firmware update</td></tr>"
             "<tr><td>/ota/upload</td><td>OTA firmware upload (POST)</td></tr>"
             "<tr><td>/restart</td><td>Reboot the device</td></tr>"
             "<tr><td>/exit</td><td>Shut down portal</td></tr>"
             "<tr><td>/erase</td><td>Erase WiFi credentials and reboot</td></tr>"
             "<tr><td>/favicon.ico</td><td>Suppress browser icon request</td></tr>"
             "<tr><td>/generate_204</td><td>Android captive portal redirect</td></tr>"
             "</table></div>",
             app_desc->version, app_desc->idf_ver, app_desc->date, app_desc->time);
    httpd_resp_sendstr_chunk(req, about);

    /* Erase + back buttons */
    httpd_resp_sendstr_chunk(req,
                             "<a class='btn warn' href='/erase'"
                             " onclick=\"return confirm('Erase all WiFi credentials and reboot?')\">"
                             "&#x1F5D1;&nbsp; Erase credentials &amp; reboot</a>"
                             "<a class='btn sec' href='/'>&#8592; Back</a>"
                             "</body></html>");

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* GET /erase — wipe entire NVS partition and reboot.
 *   Called from the confirm() dialog on the /info page.
 *   Note: erases all NVS namespaces, not just wifi_creds. */
esp_err_t erase_handler(httpd_req_t *req)
{
    const char *resp =
    PAGE_HEAD("Erasing...")
    "<div class='card'>"
    "<h2>Credentials erased</h2>"
    "<p>WiFi credentials have been wiped.<br>"
    "The device will restart and wait for new credentials.</p>"
    "</div></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;
}

/* GET /ota — OTA firmware update page
 *   Serves a file picker with a real-time upload progress bar.
 *   Uses XMLHttpRequest upload.onprogress for client-side progress — no SSE needed. */
esp_err_t ota_get_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t  *app    = esp_app_get_description();

    char current[128] = {0};
    snprintf(current, sizeof(current),
             "Running from: %s &mdash; v%s built %s %s",
             running ? running->label : "?",
             app->version, app->date, app->time);

    /* Page is built as a single string — no dynamic snprintf needed
     *       because the progress bar and JS are fully static. */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
                             "<!DOCTYPE html><html><head>"
                             "<meta charset='utf-8'>"
                             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                             "<title>WIDF Manager &#8212; OTA Update</title>"
                             "<style>"
                             "*{box-sizing:border-box;margin:0;padding:0}"
                             "body{font-family:-apple-system,sans-serif;background:#f0f2f5;"
                             "min-height:100vh;display:flex;align-items:center;"
                             "justify-content:center;padding:16px}"
                             ".card{background:#fff;border-radius:16px;"
                             "box-shadow:0 4px 24px rgba(0,0,0,.10);"
                             "padding:28px 24px;width:100%;max-width:400px}"
                             "h2{font-size:1.2rem;font-weight:700;color:#1a1a2e;margin-bottom:8px}"
                             "p{color:#888;font-size:.88rem;margin-bottom:16px;line-height:1.5}"
                             ".info{font-size:.8rem;color:#aaa;margin-bottom:20px}"
                             "label.filelbl{display:block;width:100%;padding:12px;"
                             "border:2px dashed #d0d0d0;border-radius:10px;text-align:center;"
                             "color:#888;cursor:pointer;font-size:.9rem;margin-bottom:12px;"
                             "transition:border-color .2s}"
                             "label.filelbl:hover{border-color:#4361ee;color:#4361ee}"
                             "input[type=file]{display:none}"
                             ".fname{font-size:.82rem;color:#555;margin-bottom:12px;"
                             "min-height:18px;text-align:center}"
                             ".progress-wrap{background:#f0f2f5;border-radius:8px;"
                             "overflow:hidden;height:10px;margin-bottom:10px;display:none}"
                             ".progress-bar{height:100%;background:#4361ee;width:0%;"
                             "transition:width .2s;border-radius:8px}"
                             ".pct{font-size:.82rem;color:#4361ee;text-align:center;"
                             "margin-bottom:12px;display:none}"
                             ".status{font-size:.88rem;text-align:center;min-height:20px;"
                             "margin-bottom:12px;font-weight:600}"
                             ".ok{color:#2d6a4f}.err{color:#c0392b}"
                             ".btn{display:block;width:100%;padding:12px;border:none;"
                             "border-radius:10px;font-size:.93rem;font-weight:600;"
                             "cursor:pointer;text-align:center;text-decoration:none;"
                             "margin-top:8px}"
                             ".pri{background:#4361ee;color:#fff}"
                             ".sec{background:#f0f2f5;color:#444}"
                             ".pri:disabled{background:#b0b8e0;cursor:not-allowed}"
                             "</style></head><body>"
                             "<div class='card'>"
                             "<h2>&#x2B06;&#xFE0F; OTA Firmware Update</h2>"
                             "<p>Select a <code>.bin</code> firmware file to upload.<br>"
                             "The device will ask for confirmation before rebooting.</p>");

    char info_chunk[192] = {0};
    snprintf(info_chunk, sizeof(info_chunk),
             "<p class='info'>%s</p>", current);
    httpd_resp_sendstr_chunk(req, info_chunk);

    httpd_resp_sendstr_chunk(req,
                             "<label class='filelbl' for='fw'>&#x1F4C2;&nbsp; Choose firmware .bin</label>"
                             "<input type='file' id='fw' accept='.bin'>"
                             "<div class='fname' id='fname'>No file selected</div>"
                             "<div class='progress-wrap' id='pwrap'>"
                             "<div class='progress-bar' id='pbar'></div>"
                             "</div>"
                             "<div class='pct' id='pct'>0%</div>"
                             "<div class='status' id='status'></div>"
                             "<button class='btn pri' id='upbtn' onclick='startUpload()' disabled>Upload &amp; Update</button>"
                             "<a class='btn sec' href='/'>&#8592; Back</a>"
                             "</div>"
                             "<script>"
                             "var fw=document.getElementById('fw');"
                             "fw.onchange=function(){"
                             "  var n=fw.files[0]?fw.files[0].name:'No file selected';"
                             "  document.getElementById('fname').textContent=n;"
                             "  document.getElementById('upbtn').disabled=!fw.files[0];"
                             "};"
                             "function startUpload(){"
                             "  var f=fw.files[0]; if(!f) return;"
                             "  var btn=document.getElementById('upbtn');"
                             "  btn.disabled=true;"
                             "  document.getElementById('pwrap').style.display='block';"
                             "  document.getElementById('pct').style.display='block';"
                             "  document.getElementById('status').textContent='Uploading...';"
                             "  var xhr=new XMLHttpRequest();"
                             "  xhr.open('POST','/ota/upload');"
                             "  xhr.setRequestHeader('Content-Type','application/octet-stream');"
                             "  xhr.upload.onprogress=function(e){"
                             "    if(e.lengthComputable){"
                             "      var p=Math.round(e.loaded/e.total*100);"
                             "      document.getElementById('pbar').style.width=p+'%';"
                             "      document.getElementById('pct').textContent=p+'%';"
                             "    }"
                             "  };"
                             "  xhr.onload=function(){"
                             "    if(xhr.status===200){"
                             "      document.open();document.write(xhr.responseText);document.close();"
                             "    } else {"
                             "      document.getElementById('status').className='status err';"
                             "      document.getElementById('status').textContent='Upload failed: '+xhr.responseText;"
                             "      btn.disabled=false;"
                             "    }"
                             "  };"
                             "  xhr.onerror=function(){"
                             "    document.getElementById('status').className='status err';"
                             "    document.getElementById('status').textContent='Connection error during upload';"
                             "    btn.disabled=false;"
                             "  };"
                             "  xhr.send(f);"
                             "}"
                             "</script>"
                             "</body></html>");

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* POST /ota/upload — receive raw .bin firmware, write to inactive OTA partition.
 *   Pre-checks before writing:
 *     1. Content-Length present and non-zero
 *     2. Binary fits in the target partition (hard block)
 *   Post-write checks:
 *     3. Version comparison — warns on same or older version (soft warning)
 *   On success: returns a confirmation page (user must press Reboot to apply).
 *   On failure: returns an error page, portal stays up, no partition is changed.
 *   Chunk size of 1024 bytes balances RAM usage and write speed. */
esp_err_t ota_upload_handler(httpd_req_t *req)
{
    esp_ota_handle_t      ota_handle = 0;
    const esp_partition_t *ota_part  = esp_ota_get_next_update_partition(NULL);

    if (!ota_part) {
        ESP_LOGE(TAG, "OTA: no update partition found — check partition table");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No OTA partition found. Is the partition table correct?");
        return ESP_FAIL;
    }

    /* ── Pre-check 1: content length ── */
    int total = req->content_len;
    if (total <= 0) {
        ESP_LOGE(TAG, "OTA: missing or zero Content-Length");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Missing Content-Length. Upload the file again.");
        return ESP_FAIL;
    }

    /* ── Pre-check 2: fits in partition ── */
    if ((uint32_t)total > ota_part->size) {
        ESP_LOGE(TAG, "OTA: binary %d bytes exceeds partition size %lu bytes",
                 total, (unsigned long)ota_part->size);
        char msg[128] = {0};
        snprintf(msg, sizeof(msg),
                 "Firmware too large: %d KB uploaded, partition holds %lu KB maximum.",
                 total / 1024, (unsigned long)(ota_part->size / 1024));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: writing %d KB to partition %s (capacity %lu KB)",
             total / 1024, ota_part->label,
             (unsigned long)(ota_part->size / 1024));

    esp_err_t err = esp_ota_begin(ota_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "OTA begin failed — partition may be in use.");
        return ESP_FAIL;
    }

    char buf[1024];
    int  received  = 0;
    bool write_err = false;

    while (received < total) {
        int chunk = httpd_req_recv(req, buf, sizeof(buf));
        if (chunk < 0) {
            ESP_LOGE(TAG, "OTA: receive error");
            write_err = true;
            break;
        }
        if (chunk == 0) break;   /* connection closed */

        err = esp_ota_write(ota_handle, buf, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write error: %s", esp_err_to_name(err));
            write_err = true;
            break;
        }
        received += chunk;
        ESP_LOGD(TAG, "OTA: %d / %d bytes written", received, total);
    }

    if (write_err || received != total) {
        esp_ota_abort(ota_handle);
        ESP_LOGE(TAG, "OTA: aborted after %d / %d bytes", received, total);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req,
                           "Upload failed or incomplete. The firmware was not applied. "
                           "Check the file and try again.");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req,
                           "Firmware validation failed — the binary may be corrupt or invalid. "
                           "Check the file and try again.");
        return ESP_FAIL;
    }

    /* ── Post-check 3: version comparison ── */
    const esp_app_desc_t *running_desc = esp_app_get_description();
    esp_app_desc_t        new_desc     = {0};
    bool version_warn = false;
    char version_msg[128] = {0};

    if (esp_ota_get_partition_description(ota_part, &new_desc) == ESP_OK) {
        int cmp = strcmp(new_desc.version, running_desc->version);
        if (cmp == 0) {
            version_warn = true;
            snprintf(version_msg, sizeof(version_msg),
                     "&#x26A0;&#xFE0F; Same version (%s) is already running.",
                     new_desc.version);
            ESP_LOGW(TAG, "OTA: uploaded version %s matches running version",
                     new_desc.version);
        } else if (cmp < 0) {
            version_warn = true;
            snprintf(version_msg, sizeof(version_msg),
                     "&#x26A0;&#xFE0F; This is a downgrade: v%s &rarr; v%s.",
                     running_desc->version, new_desc.version);
            ESP_LOGW(TAG, "OTA: downgrade detected — running %s, uploading %s",
                     running_desc->version, new_desc.version);
        } else {
            ESP_LOGI(TAG, "OTA: upgrade v%s -> v%s",
                     running_desc->version, new_desc.version);
        }
    }

    err = esp_ota_set_boot_partition(ota_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Failed to set boot partition.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: success — %d bytes written to %s. Awaiting reboot.",
             received, ota_part->label);

    /* Success page — show version warning if applicable */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        PAGE_HEAD("Update Ready")
        "<div class='card'>"
        "<h2 style='color:#2d6a4f'>&#10003; Update Ready</h2>"
        "<p>Firmware uploaded successfully.<br>"
        "Press Reboot to apply the update.<br>"
        "The device will roll back automatically if the new firmware fails to boot.</p>");

    if (version_warn) {
        char warn_chunk[192] = {0};
        snprintf(warn_chunk, sizeof(warn_chunk),
                 "<p style='color:#b5590a;font-size:.85rem;margin-top:8px'>%s</p>",
                 version_msg);
        httpd_resp_sendstr_chunk(req, warn_chunk);
    }

    httpd_resp_sendstr_chunk(req,
        "<a class='btn pri' href='/restart'>&#x1F504;&nbsp; Reboot now</a>"
        "<a class='btn sec' href='/'>&#8592; Back to menu</a>"
        "</div></body></html>");

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* GET /restart — send confirmation page then reboot after 500 ms */
esp_err_t restart_handler(httpd_req_t *req)
{
    const char *resp =
    PAGE_HEAD("Rebooting")
    "<div class='card'>"
    "<h2>Rebooting...</h2>"
    "<p>The device will restart momentarily.</p>"
    "</div></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* GET /exit — send "portal closed" page, set exit flag.
 *   httpd_stop() cannot be called from inside a handler (deadlock) so we
 *   set g_exit_requested = true and let portal_run() in main stop the server
 *   after this handler returns. */
esp_err_t exit_handler(httpd_req_t *req)
{
    const char *resp =
    PAGE_HEAD("Portal Closed")
    "<div class='card'>"
    "<h2>Portal closed</h2>"
    "<p>The configuration portal has been shut down.<br>"
    "Long press the button to reopen it.</p>"
    "</div></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, resp);
    g_exit_requested = true;   /* portal_run() polls this and stops the server */
    return ESP_OK;
}

/* ── Web Server ──────────────────────────────────────────────────────────── */
/* Starts the HTTP server, registers all 9 URI handlers, stores handle in
 *   g_server so exit_handler can signal a stop via g_exit_requested.
 *   stack_size bumped to 8192 for the info page data gathering.
 *   max_uri_handlers set to 16 for future expansion. */
httpd_handle_t start_webserver(void)
{
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = 8192;
    config.max_uri_handlers = 16;

    if (httpd_start(&g_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        g_server = NULL;
        return NULL;
    }

    httpd_uri_t menu_uri    = { .uri = "/",             .method = HTTP_GET,  .handler = menu_get_handler    };
    httpd_uri_t portal_uri  = { .uri = "/wifi",         .method = HTTP_GET,  .handler = portal_get_handler  };
    httpd_uri_t refresh_uri = { .uri = "/wifi/refresh", .method = HTTP_GET,  .handler = wifi_refresh_handler};
    httpd_uri_t save_uri    = { .uri = "/save",         .method = HTTP_POST, .handler = save_post_handler   };
    httpd_uri_t info_uri    = { .uri = "/info",         .method = HTTP_GET,  .handler = info_get_handler    };
    httpd_uri_t erase_uri   = { .uri = "/erase",        .method = HTTP_GET,  .handler = erase_handler       };
    httpd_uri_t ota_uri        = { .uri = "/ota",         .method = HTTP_GET,  .handler = ota_get_handler    };
    httpd_uri_t ota_upload_uri = { .uri = "/ota/upload",   .method = HTTP_POST, .handler = ota_upload_handler };
    httpd_uri_t restart_uri = { .uri = "/restart",      .method = HTTP_GET,  .handler = restart_handler     };
    httpd_uri_t exit_uri    = { .uri = "/exit",         .method = HTTP_GET,  .handler = exit_handler        };
    httpd_uri_t captive_uri = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler };
    httpd_uri_t favicon_uri = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler };

    httpd_register_uri_handler(g_server, &favicon_uri);
    httpd_register_uri_handler(g_server, &captive_uri);
    httpd_register_uri_handler(g_server, &menu_uri);
    httpd_register_uri_handler(g_server, &portal_uri);
    httpd_register_uri_handler(g_server, &refresh_uri);
    httpd_register_uri_handler(g_server, &save_uri);
    httpd_register_uri_handler(g_server, &info_uri);
    httpd_register_uri_handler(g_server, &erase_uri);
    httpd_register_uri_handler(g_server, &ota_uri);
    httpd_register_uri_handler(g_server, &ota_upload_uri);
    httpd_register_uri_handler(g_server, &restart_uri);
    httpd_register_uri_handler(g_server, &exit_uri);

    ESP_LOGI(TAG, "HTTP server started — 13 routes registered");
    return g_server;
}

/* ── End of widf_mngr_handlers.c ─────────────────────────────────────────── */
