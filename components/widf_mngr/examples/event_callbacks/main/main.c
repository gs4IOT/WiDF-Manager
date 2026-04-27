/*  event_callbacks — WIDF Manager event callback example
 *
 *  Demonstrates how to receive lifecycle events from WIDF Manager.
 *  The callback is called for every state transition — use it to
 *  update UI, trigger application logic, or log device status.
 *
 *  Events fired:
 *    WIDF_EVENT_STA_TRYING        — attempting to connect to a network
 *    WIDF_EVENT_STA_CONNECTED     — STA connected and IP acquired
 *    WIDF_EVENT_STA_DISCONNECTED  — STA lost connection mid-session
 *    WIDF_EVENT_STA_FAILED        — all retries exhausted for a network
 *    WIDF_EVENT_PORTAL_OPENED     — portal HTTP server started
 *    WIDF_EVENT_PORTAL_CLOSED     — portal HTTP server stopped
 *    WIDF_EVENT_CREDENTIALS_SAVED — new credentials written to NVS
 */

#include <string.h>
#include "widf_mngr.h"
#include "esp_log.h"

static const char *TAG = "app";

/* Event callback — receives all WIDF Manager lifecycle events.
 * Called from the widf_mngr task context. Keep it non-blocking. */
static void on_widf_event(const widf_event_data_t *evt)
{
    switch (evt->event) {

        case WIDF_EVENT_STA_TRYING:
            /* Fired once per network before retries begin.
             * evt->data.trying.retries is the retry budget for this network. */
            ESP_LOGI(TAG, "Connecting to: %s (%d retries budget)",
                     evt->data.trying.ssid, evt->data.trying.retries);
            break;

        case WIDF_EVENT_STA_CONNECTED:
            /* Fired when STA acquires an IP address. */
            ESP_LOGI(TAG, "Connected — SSID: %s  IP: %s",
                     evt->data.connected.ssid, evt->data.connected.ip);
            break;

        case WIDF_EVENT_STA_DISCONNECTED:
            /* Fired on mid-session drop — not on a failed connect attempt. */
            ESP_LOGW(TAG, "Connection lost: %s", evt->data.disconnected.ssid);
            break;

        case WIDF_EVENT_STA_FAILED:
            /* Fired after all retries for a network are exhausted. */
            ESP_LOGE(TAG, "Failed: %s after %d retries",
                     evt->data.failed.ssid, evt->data.failed.retries);
            break;

        case WIDF_EVENT_PORTAL_OPENED:
            /* Fired after the HTTP server starts and the portal is ready. */
            ESP_LOGI(TAG, "Portal open — connect to AP and browse to 192.168.4.1");
            break;

        case WIDF_EVENT_PORTAL_CLOSED:
            /* Fired after the HTTP server stops (timeout or /exit). */
            ESP_LOGI(TAG, "Portal closed");
            break;

        case WIDF_EVENT_CREDENTIALS_SAVED:
            /* Fired immediately after new credentials are written to NVS.
             * Reconnect attempt follows if on_save_mode == WIDF_ON_SAVE_RECONNECT. */
            ESP_LOGI(TAG, "Credentials saved for: %s", evt->data.saved.ssid);
            break;
    }
}

void app_main(void)
{
    widf_mngr_config_t cfg = WIDF_MNGR_DEFAULT_CONFIG();

    /* Register the event callback */
    cfg.on_event = on_widf_event;

    widf_mngr_run(&cfg);
}
